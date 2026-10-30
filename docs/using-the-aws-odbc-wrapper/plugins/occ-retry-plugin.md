## OCC Retry Plugin for the AWS Advanced ODBC Wrapper

The OCC Retry Plugin runs a statement again when it fails with an [Amazon Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html) optimistic concurrency control (OCC) conflict. It retries only a single statement that runs on its own in autocommit mode. It never retries a statement inside a transaction. The plugin is off by default and works only on Aurora DSQL connections.

### What is an OCC Conflict?

Aurora DSQL doesn't lock rows. It checks for conflicts when a transaction commits ([Concurrency control in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-concurrency-control.html)):

- When two transactions change the same row, the one that commits second fails with SQLSTATE `40001` and `(OC000)` in the message.
- After another session changes the schema, for example with `ALTER TABLE`, `GRANT` or `REVOKE`, a statement can fail with SQLSTATE `40001` and `(OC001)` in the message.

A statement that runs on its own in autocommit mode is its own transaction. After an OCC conflict it has changed nothing, and running it again usually succeeds.

### Connection String / DSN Configuration for OCC Retry Plugin Support

| Connection Option Key | Description                                                                                                                            | Valid Values                                | Default Value | Sample Value |
|-----------------------|----------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------|---------------|--------------|
| `ENABLE_OCC_RETRY`    | Set to `1` to retry statements that fail with an OCC conflict.                                                                         | `0` or `1`                                  | `0`           | `1`          |
| `OCC_MAX_RETRIES`     | Retries after the first attempt, so a statement runs at most `1 + OCC_MAX_RETRIES` times. `0` turns retries off.                       | integer from `0` to `100`                   | `3`           | `5`          |
| `OCC_BASE_DELAY_MS`   | Wait before the first retry, in milliseconds. The wait doubles for each later retry.                                                   | integer from `1` to `60000`                 | `100`         | `50`         |
| `OCC_MAX_DELAY_MS`    | Longest wait before a retry, in milliseconds, before jitter.                                                                           | integer from `OCC_BASE_DELAY_MS` to `60000` | `5000`        | `2000`       |
| `OCC_JITTER_FACTOR`   | Random extra wait, as a fraction of the wait: `0.25` adds up to 25%. Write it with a decimal point, whatever the application's locale. | number from `0.0` to `1.0`                  | `0.25`        | `0.5`        |

The wrapper checks all five options on every connect, also when `ENABLE_OCC_RETRY=0`. An invalid value fails the connect with `SQL_ERROR`, SQLSTATE `01S00` and `Invalid value specified for connection string attribute:` followed by the option, its value and the valid values, for example `OCC_MAX_RETRIES=101 (use an integer from 0 to 100)`. Only the first invalid OCC option is reported.

`OCC_MAX_DELAY_MS` must be at least `OCC_BASE_DELAY_MS`, also when you don't set it: `OCC_BASE_DELAY_MS=10000` alone fails with `OCC_MAX_DELAY_MS=5000 (the default; set it to an integer from OCC_BASE_DELAY_MS, 10000, to 60000)`.

The wrapper reads the options when a connection handle connects for the first time. To change them, use a new connection handle.

> [!NOTE]\
> The Windows DSN setup dialog has no fields for these options. Set them in the connection string, in `odbc.ini` on Linux and macOS, or, in a Windows DSN, in **Base Driver Connection String**, for example `ENABLE_OCC_RETRY=1;OCC_MAX_RETRIES=5`.

### Requirements

- An Aurora DSQL connection: `RDS_AUTH_TYPE=DSQL`, an Aurora DSQL endpoint in `SERVER`, or `DATABASE_DIALECT=AURORA_DSQL`, for example with AWS PrivateLink. See [When the Wrapper Treats a Connection as Aurora DSQL](../SupportForAuroraDSQL.md#when-the-wrapper-treats-a-connection-as-aurora-dsql). On any other connection, `ENABLE_OCC_RETRY=1` fails the connect with `SQL_ERROR`, SQLSTATE `08001` and this message:

  ```
  ENABLE_OCC_RETRY=1 works only with Aurora DSQL. Use an Aurora DSQL endpoint as SERVER, or set RDS_AUTH_TYPE=DSQL or DATABASE_DIALECT=AURORA_DSQL; otherwise set ENABLE_OCC_RETRY=0.
  ```

- psqlODBC as the underlying driver.

### When a Statement Is Retried

The plugin retries a failed statement only when all of these hold:

1. `OCC_MAX_RETRIES` is above `0`.
2. Autocommit is on, and no transaction is open. See [How the Plugin Tracks Transactions](#how-the-plugin-tracks-transactions).
3. The statement ran through `SQLExecDirect` or `SQLExecDirectW`, or through `SQLPrepare` or `SQLPrepareW` and then `SQLExecute`.
4. The text is a single statement: it has no `;` except at the end. A `;` inside a string literal or a comment counts too, so `UPDATE t SET s = 'a;b'` isn't retried.
5. Its first word, after any whitespace and comments, isn't `ABORT`, `BEGIN`, `COMMIT`, `END`, `PREPARE`, `RELEASE`, `ROLLBACK`, `SAVEPOINT`, `SET` or `START`, in any case.
6. It has one parameter set: `SQL_ATTR_PARAMSET_SIZE` is `1`.
7. The call returned `SQL_ERROR`, and the first diagnostic record has SQLSTATE `40001`, `OC000` or `OC001`.

Each retry waits (see [Backoff](#backoff)), clears the statement's diagnostics, and runs the statement again on the same statement handle. The first result that isn't `SQL_ERROR` is returned. A retry that fails with another error returns that error. When the retries run out, the call returns `SQL_ERROR` with the last attempt's diagnostics, so the application still gets SQLSTATE `40001`.

### What Is Never Retried

- Statements in a transaction: with autocommit off, or after a `BEGIN` or `START TRANSACTION`. Running one statement of a transaction again on its own would split the transaction. In a transaction, an `OC000` conflict surfaces at the `COMMIT`. See [Retrying Transactions](#retrying-transactions).
- Batches, transaction control statements and `SET` statements.
- Parameter arrays (`SQL_ATTR_PARAMSET_SIZE` above `1`, or `SQLParamOptions`), and data-at-execution parameters (`SQLParamData` and `SQLPutData`).
- Errors from `SQLPrepare`, `SQLEndTran`, `SQLFetch`, `SQLFetchScroll`, `SQLExtendedFetch`, `SQLMoreResults`, `SQLBulkOperations`, `SQLSetPos` and the catalog functions.
- Any other SQLSTATE, even when the message contains `(OC000)` or `(OC001)`.

### Backoff

Before retry *n*, the plugin waits `min(OCC_BASE_DELAY_MS × 2^(n-1), OCC_MAX_DELAY_MS)` milliseconds, plus a random extra of up to `OCC_JITTER_FACTOR` times that. There's no wait after the last attempt. With the defaults the waits are 100-124 ms, 200-249 ms and 400-499 ms, under 0.9 seconds in all.

The plugin waits inside the ODBC call and holds the connection meanwhile: other threads that use the connection wait, and an `SQLCancel` from another thread waits until the call returns. With `OCC_MAX_RETRIES=100` and the default `OCC_MAX_DELAY_MS`, the waits add up to about 10 minutes at most; with every option at its limit, to about 3.3 hours at most. Keep `OCC_MAX_RETRIES` small.

### How the Plugin Tracks Transactions

psqlODBC doesn't tell the wrapper whether a transaction is open, so while autocommit is on, the plugin follows the SQL it runs:

- A transaction opens when a statement starts with `BEGIN` or `START`, or with `COMMIT`, `END`, `ABORT` or `ROLLBACK` and `AND CHAIN`. This holds in any form: prepared, after whitespace or comments, or after a `;` in a batch such as `SELECT 1; BEGIN`. It holds even when the statement fails.
- The transaction ends with a statement that is only `COMMIT`, `END`, `ABORT` or `ROLLBACK`, optionally with `WORK`, `TRANSACTION` or `AND NO CHAIN`, when that statement succeeds. It also ends with `SQLEndTran`, when autocommit is set off and back on, and when the handle connects again.

A `BEGIN` after a `;` inside a string literal or a comment counts as opening a transaction. A `COMMIT` followed by `;` and a comment, a `COMMIT` in a batch and a `COMMIT` that failed don't end one. Either way, retries stay off until the transaction ends in one of the ways above.

For transactions, set `SQL_ATTR_AUTOCOMMIT` to `SQL_AUTOCOMMIT_OFF` with `SQLSetConnectAttr` and end them with `SQLEndTran`, rather than sending `BEGIN` and `COMMIT`.

#### Transactions the Plugin Can Miss

The plugin's view comes from the SQL it runs, not from the server. It can miss a transaction in two cases:

- The connection is enlisted in a Windows MSDTC transaction (`SQL_ATTR_ENLIST_IN_DTC`).
- A batch starts a transaction after another statement, such as `SELECT 1; BEGIN`, and the application then calls `SQLEndTran`. psqlODBC sends no `COMMIT` for that transaction, so it stays open on the server, while the plugin treats it as ended.

If a statement in such a transaction fails with an `OC001` conflict, the plugin retries it. Depending on whether Aurora DSQL has already ended the failed transaction, the retry either fails with SQLSTATE `25P02` and changes nothing, or commits that one statement without the rest of the transaction.

To avoid them, don't enlist the connection in MSDTC with the plugin on, and don't start a transaction inside a batch.

### Retrying Transactions

The plugin doesn't retry transactions. Retry them in the application: when a statement or the commit fails with SQLSTATE `40001`, roll back and run the whole transaction again from its first statement, after a short wait with random jitter. For example, with autocommit off:

```cpp
#include <sql.h>
#include <sqlext.h>

#include <chrono>
#include <cstring>
#include <random>
#include <thread>

// True when the last call on the handle failed with SQLSTATE 40001, an Aurora DSQL OCC conflict.
bool IsOccConflict(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR state[6] = {0};
    SQLINTEGER native_error = 0;
    SQLCHAR message[512] = {0};
    SQLSMALLINT length = 0;
    const SQLRETURN ret = SQLGetDiagRec(handle_type, handle, 1, state, &native_error, message, sizeof(message), &length);
    return SQL_SUCCEEDED(ret) && std::strcmp(reinterpret_cast<const char*>(state), "40001") == 0;
}

// Moves 10 from account 1 to account 2 in one transaction. After an OCC conflict it runs the whole transaction again,
// up to 4 attempts in all, waiting 100, 200 and 400 ms plus up to 25% random jitter before the next attempt.
SQLRETURN Transfer(SQLHDBC dbc, SQLHSTMT stmt)
{
    SQLCHAR debit[] = "UPDATE accounts SET balance = balance - 10 WHERE id = 1";
    SQLCHAR credit[] = "UPDATE accounts SET balance = balance + 10 WHERE id = 2";
    constexpr int MAX_ATTEMPTS = 4;
    std::mt19937 generator(std::random_device{}());
    for (int attempt = 1;; ++attempt) {
        bool conflict = false;
        SQLRETURN ret = SQLExecDirect(stmt, debit, SQL_NTS);
        if (SQL_SUCCEEDED(ret)) {
            ret = SQLExecDirect(stmt, credit, SQL_NTS);
        }
        if (SQL_SUCCEEDED(ret)) {
            ret = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
            if (SQL_SUCCEEDED(ret)) {
                return ret;
            }
            conflict = IsOccConflict(SQL_HANDLE_DBC, dbc);
        } else {
            conflict = IsOccConflict(SQL_HANDLE_STMT, stmt);
        }
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        if (!conflict || attempt == MAX_ATTEMPTS) {
            return ret;
        }
        const int delay_ms = 100 << (attempt - 1);
        std::uniform_int_distribution<int> jitter(0, delay_ms / 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms + jitter(generator)));
    }
}
```

> [!WARNING]\
> Don't put autocommit statements in your own retry loop while the plugin is on. The attempts multiply: up to your loop's attempts × (1 + `OCC_MAX_RETRIES`). Use your loop for transactions only, or set `ENABLE_OCC_RETRY=0`.

End a transaction with `SQLEndTran` before you set autocommit on. If a transaction is open when autocommit is set on, psqlODBC commits it but doesn't report the result, so the application isn't told when that commit fails with an OCC conflict.

A retried `INSERT` that takes a value from a sequence takes a new value, because sequence values aren't rolled back, so retries can leave larger gaps.

### Logging

The plugin writes these lines to the wrapper's log (see [Logging](../using-the-aws-odbc-wrapper.md#logging)):

| Level   | When                     | Line                                                                                              |
|---------|--------------------------|---------------------------------------------------------------------------------------------------|
| INFO    | Before each retry        | `[DSQL] OCC conflict (<code>, SQLSTATE <state>): retry <n>/<N> in <ms> ms`                        |
| WARNING | When the retries run out | `[DSQL] OCC conflict (<code>, SQLSTATE <state>) persisted after <N> retries; returning the error` |

`<code>` is `OC000` or `OC001` when the SQLSTATE or the message names it; otherwise the parentheses hold only `SQLSTATE <state>`. For example: `[DSQL] OCC conflict (OC000, SQLSTATE 40001): retry 1/3 in 117 ms`. Nothing is logged for a conflict that isn't retried, or with `OCC_MAX_RETRIES=0`.

### Sample Connection String

With the Unicode wrapper on Linux:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;ENABLE_OCC_RETRY=1;OCC_MAX_RETRIES=5;
```

### See Also

- [Support for Amazon Aurora DSQL](../SupportForAuroraDSQL.md): what the wrapper changes on Aurora DSQL connections, and the Aurora DSQL behaviors the application handles.
