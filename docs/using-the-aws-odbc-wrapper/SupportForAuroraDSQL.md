# Support for Amazon Aurora DSQL

The AWS Advanced ODBC Wrapper supports [Amazon Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html) with [psqlODBC](https://github.com/postgresql-interfaces/psqlodbc) as the underlying driver. Aurora DSQL is PostgreSQL-compatible, but it doesn't support some features that psqlODBC uses: savepoints, cursors, `SET statement_timeout` and `SET SESSION CHARACTERISTICS`. On an Aurora DSQL connection, the wrapper changes psqlODBC settings and answers some ODBC calls itself, so that psqlODBC doesn't send statements Aurora DSQL rejects. It also rejects settings that can't work with Aurora DSQL.

This page lists those changes, and the Aurora DSQL behaviors the application must handle.

## General Usage

Sample connection string, with the Unicode wrapper on Linux:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

`RDS_AUTH_TYPE=DSQL` makes the wrapper sign an Aurora DSQL authentication token for each connection. For its options, TLS, region and cluster IDs, see the [Aurora DSQL Authentication Plugin](./plugins/aurora-dsql-authentication-plugin.md).

The warnings on this page go to the wrapper's log, in the `aws-odbc-wrapper` folder of the user's temporary directory (see [Logging](./using-the-aws-odbc-wrapper.md#logging)).

## When the Wrapper Treats a Connection as Aurora DSQL

The wrapper treats a connection as Aurora DSQL when any of these holds (keys and values are case-insensitive):

- `RDS_AUTH_TYPE=DSQL`
- `SERVER` is an Aurora DSQL endpoint, such as `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws`
- `DATABASE_DIALECT=AURORA_DSQL`

Everything on this page applies to such a connection, whatever its authentication type. The wrapper decides this at a connection handle's first connect and keeps it for the handle: a later connect of the same handle to the other kind of database fails (see [A Handle Used for Another Database](#a-handle-used-for-another-database)).

- With AWS PrivateLink, a DNS alias or an IP address in `SERVER`, and an authentication type other than `DSQL` (for example `DATABASE`, with a token you generate as `PWD`), set `DATABASE_DIALECT=AURORA_DSQL`, and an `SSLmode` as in [TLS](#tls). Otherwise the wrapper treats the connection as Aurora PostgreSQL, and nothing on this page applies.
- Don't set `DATABASE_DIALECT=AURORA_DSQL` for an RDS or Aurora PostgreSQL database, for example to avoid topology queries: it turns on everything on this page.
- When `DATABASE_DIALECT` names another dialect on an Aurora DSQL connection, the wrapper uses that dialect, which expects Aurora or RDS topology, but still applies this page. It logs a warning on each connect, for example:

  ```
  [DSQL] DATABASE_DIALECT=AURORA_POSTGRESQL on an Aurora DSQL connection (SERVER is an Aurora DSQL endpoint): the driver uses that dialect, which expects Aurora or RDS topology. Remove DATABASE_DIALECT or set it to AURORA_DSQL.
  ```

## psqlODBC Settings the Wrapper Sets

On every connect, the wrapper sets five psqlODBC options. Its value replaces yours wherever you set it: in the connection string, `BASE_CONN` or the base DSN, in the long or the short form, or in psqlODBC's `CX` flags. When your value differs, the wrapper logs a warning.

| psqlODBC Option                          | Set To  | Why                                                                                                          | Warning When Your Value Differs                                                                                                                                       |
|------------------------------------------|---------|--------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Protocol` (short form `A1`)             | `7.4-0` | Other rollback levels make psqlODBC send `SAVEPOINT`, which Aurora DSQL doesn't support.                     | `[DSQL] Overriding Protocol=<value> with 7.4-0: Aurora DSQL doesn't support savepoints`                                                                               |
| `IgnoreTimeout` (short form `D9`)        | `1`     | A query timeout makes psqlODBC send `SET statement_timeout`, which Aurora DSQL doesn't support.              | `[DSQL] Overriding IgnoreTimeout=<value> with 1: Aurora DSQL doesn't support statement_timeout`                                                                       |
| `UseDeclareFetch` (short form `B6`)      | `0`     | With `1`, psqlODBC reads results through `DECLARE ... CURSOR`, which Aurora DSQL doesn't support.            | `[DSQL] Overriding UseDeclareFetch=<value> with 0: Aurora DSQL doesn't support cursors`                                                                               |
| `UseServerSidePrepare` (short form `C8`) | `0`     | See [Server-Side Prepare](#server-side-prepare).                                                             | `[DSQL] Overriding UseServerSidePrepare=<value> with 0: freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL`      |
| `UpdatableCursors` (short form `C4`)     | `0`     | With `1`, keyset-driven and updatable cursors make psqlODBC select `ctid`, which Aurora DSQL doesn't return. | `[DSQL] Overriding UpdatableCursors=<value> with 0: Aurora DSQL doesn't return system columns such as ctid, which psqlODBC's keyset-driven and updatable cursors use` |

`<value>` is your value. A value set only in `CX` is replaced without a warning.

psqlODBC's connection attributes `SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE`, `SQL_ATTR_PGOPT_USE_DECLAREFETCH` and `SQL_ATTR_PGOPT_IGNORETIMEOUT` would undo three of these options. On an open Aurora DSQL connection, setting one of them to another value returns `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02`, for example `SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL`, and the option keeps the wrapper's value. A value set before connecting is dropped at connect, with a warning in the log: `[DSQL] Ignoring SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE=<n> set before connect: ...`.

### TLS

With `RDS_AUTH_TYPE=DSQL`, the [Aurora DSQL Authentication Plugin](./plugins/aurora-dsql-authentication-plugin.md#tls) sets TLS. With another authentication type, an `SSLmode` that psqlODBC doesn't run as `disable`, such as `allow`, `prefer`, `require`, `verify-ca` or `verify-full`, is left exactly as you set it: the wrapper adds no root certificate and no `application_name` to `pqopt`. Only `verify-full` checks the host name.

An `SSLmode` that psqlODBC would run as `disable` (not set, `disable`, or a value that doesn't start with `a`, `p`, `r` or `v`: psqlODBC reads only the first letter, case-sensitively) becomes `verify-full`, with the same root certificate default as the Aurora DSQL plugin (see [Root Certificates](./plugins/aurora-dsql-authentication-plugin.md#root-certificates)); a full mode name in another case, such as `Require`, which psqlODBC also runs as `disable`, becomes that mode in lowercase instead. When the wrapper replaces a value you set with `verify-full`, it logs `[DSQL] Overriding SSLmode=<value> with verify-full: psqlODBC would connect without TLS, which Aurora DSQL refuses` at WARNING level, where `<value>` is your value, and the connect returns `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` (see [SSL mode changed](./plugins/aurora-dsql-authentication-plugin.md#ssl-mode-changed)), unless it returns another warning. When it lowercases a mode, it logs `[DSQL] Lowercasing SSLmode=<value> to <mode>: psqlODBC reads the mode case-sensitively and would connect without TLS` at INFO level, where `<mode>` is the lowercase mode, and the connect returns no warning for it.

With an IP address or a PrivateLink name in `SERVER`, `verify-full` fails: set `SSLmode=verify-ca` with a root certificate file, or `require`, and the cluster ID in libpq's `options`, as in [AWS PrivateLink Without Private DNS](./plugins/aurora-dsql-authentication-plugin.md#aws-privatelink-without-private-dns).

### Savepoints and Errors in a Transaction

With `Protocol=7.4-0`, psqlODBC doesn't roll back a failed statement inside a transaction, and Aurora DSQL ends the whole transaction at the first error: later statements fail with SQLSTATE `25P02`. After an error in a transaction, call `SQLEndTran` with `SQL_ROLLBACK` and run the transaction again.

If the application commits instead, and a call to `SQLExecDirect`, `SQLExecute` or `SQLParamData` in the transaction failed or returned `SQL_SUCCESS_WITH_INFO`, the wrapper first runs `SELECT 1`. When that fails with `25P02`, the wrapper rolls the transaction back, and `SQLEndTran` returns `SQL_ERROR` with SQLSTATE `25S03` and `The transaction was rolled back, not committed: a statement in it failed, and Aurora DSQL ends a transaction at its first error (it has no savepoints). Roll back after an error, and run the whole transaction again.` When the check fails otherwise, the wrapper commits as psqlODBC would and logs a warning. A failed catalog function doesn't start the check.

Setting `SQL_ATTR_AUTOCOMMIT` on or off while a transaction is open commits it first, with the same check. When that commit fails, for example with a conflict (`40001`) or `25S03`, `SQLSetConnectAttr` returns its error and autocommit stays as it was.

Run transactions with autocommit off, and end them with `SQLEndTran`. After an error, a `COMMIT` sent as SQL text still returns success and saves nothing. After a `BEGIN` sent as SQL text with autocommit on, the Windows driver manager doesn't pass `SQLEndTran` to the wrapper, so no check runs there.

`SAVEPOINT`, `RELEASE SAVEPOINT` and `ROLLBACK TO SAVEPOINT` statements that the application sends fail with Aurora DSQL's error.

### Server-Side Prepare

With psqlODBC's default, `UseServerSidePrepare=1`, a prepared statement gets a named plan on the server. When the application frees such a statement, or prepares it again, inside a transaction, psqlODBC sends `DEALLOCATE` for the plan and ignores the result. psqlODBC versions before 18.00.0003 wrap that `DEALLOCATE` in a `SAVEPOINT`. At `Protocol=7.4-0`, which the wrapper sets, Aurora DSQL rejects the `SAVEPOINT`, which aborts the transaction, and psqlODBC doesn't report the error: every call, the `COMMIT` included, returns success, but the transaction's changes are lost. (At psqlODBC's default level, the first statement with parameters in a transaction fails with `internal SAVEPOINT failed` instead.) psqlODBC 18.00.0003 and later send the `DEALLOCATE` alone at `Protocol=7.4-0`; if Aurora DSQL rejects it, psqlODBC rolls the transaction back without reporting it, and the statements after it commit without the ones before it. A common case is a statement with parameters followed by a commit, such as pyodbc's `connection.execute("UPDATE t SET v = ? WHERE id = ?", 1, 2)` and then `connection.commit()`. In autocommit mode nothing is lost.

With `UseServerSidePrepare=0`, psqlODBC puts the parameter values into the statement text itself, so it creates and frees no plans. This has costs:

- Every `SQLExecute` sends the whole statement, and Aurora DSQL plans it each time.
- `SQLDescribeParam` fails for a parameter that isn't bound yet, with `Unfortunately couldn't get this paramater's info`. pyodbc, for example, calls it only for `None` values, and then uses `SQL_VARCHAR`.

`SQLNumResultCols` and `SQLDescribeCol` before `SQLExecute` still work, and a schema change no longer makes a prepared statement fail with `cached plan must not change result type`.

psqlODBC's keyset-driven and updatable cursors use the `ctid` column, which Aurora DSQL doesn't return, so the wrapper sets `UpdatableCursors=0`. psqlODBC then makes a keyset-driven or dynamic cursor static and an updatable concurrency read-only: `SQLSetStmtAttr` returns `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02`. `SQLSetPos` with `SQL_UPDATE`, `SQL_DELETE` or `SQL_ADD` and `SQLBulkOperations` fail: change rows with `UPDATE`, `DELETE` and `INSERT` statements.

### Large Results

With `UseDeclareFetch=0`, psqlODBC reads a query's whole result into memory when the statement runs. Read large results in pages, for example with `LIMIT` and a `WHERE` condition on the last key read.

## ODBC Calls the Wrapper Answers

On an open Aurora DSQL connection, the wrapper answers some calls itself, so that psqlODBC doesn't send statements Aurora DSQL rejects.

### Query Timeout

| Call                                                                                                                  | Result                                                                                                                                                                                                                                                                                                                                                            |
|-----------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SQLSetStmtAttr` with `SQL_ATTR_QUERY_TIMEOUT` above `0`                                                              | `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` and `SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it limits each transaction to 5 minutes)`. The timeout stays `0`, which `SQLGetStmtAttr` returns, unless a timeout was set on the connection before connecting: that one is kept, without a warning, and psqlODBC ignores it. |
| `SQLSetConnectAttr` with `SQL_ATTR_QUERY_TIMEOUT` above `0`, or ODBC 2 `SQLSetConnectOption` with `SQL_QUERY_TIMEOUT` | The same, on the connection handle.                                                                                                                                                                                                                                                                                                                               |
| Either call with `0`                                                                                                  | Passed to psqlODBC.                                                                                                                                                                                                                                                                                                                                               |
| ODBC 2 `SQLSetStmtOption` with `SQL_QUERY_TIMEOUT`                                                                    | Passed to psqlODBC, which ignores it because of `IgnoreTimeout=1`. This needs psqlODBC 12.02.0000 or later.                                                                                                                                                                                                                                                       |

The wrapper logs the message at INFO level, with the value asked for: `[DSQL] <message> (requested <n>)`. Aurora DSQL ends a transaction that runs longer than 5 minutes ([Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html)).

### Transaction Isolation

Aurora DSQL runs every transaction at `REPEATABLE READ`, which is snapshot isolation ([Concurrency control in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-concurrency-control.html)). Snapshot isolation allows write skew: two transactions that read the same rows and each change different ones can both commit. To make them conflict, read the rows that a decision depends on with `SELECT ... FOR UPDATE`.

| Call                                                                                                                                         | Result                                                                                                                                                                                                                                                                            |
|----------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SQLSetConnectAttr` with `SQL_ATTR_TXN_ISOLATION` set to `SQL_TXN_REPEATABLE_READ`, or ODBC 2 `SQLSetConnectOption` with `SQL_TXN_ISOLATION` | `SQL_SUCCESS`                                                                                                                                                                                                                                                                     |
| The same with `SQL_TXN_READ_UNCOMMITTED` or `SQL_TXN_READ_COMMITTED`                                                                         | `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` and `SQL_ATTR_TXN_ISOLATION changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at REPEATABLE READ`, logged at WARNING level                                                                                     |
| The same with `SQL_TXN_SERIALIZABLE`                                                                                                         | `SQL_ERROR` with SQLSTATE `HY024` and `SQL_TXN_SERIALIZABLE isn't supported: Aurora DSQL runs every transaction at REPEATABLE READ (snapshot isolation). Use SQL_TXN_REPEATABLE_READ, and read the rows a decision depends on with SELECT ... FOR UPDATE.`, logged at ERROR level |
| The same with any other value                                                                                                                | `SQL_ERROR` with SQLSTATE `HY024` and `Invalid SQL_ATTR_TXN_ISOLATION value <n>`, logged at ERROR level                                                                                                                                                                           |
| `SQLGetInfo` with `SQL_TXN_ISOLATION_OPTION`                                                                                                 | `SQL_TXN_REPEATABLE_READ`, the only level supported                                                                                                                                                                                                                               |

`SQLGetConnectAttr` with `SQL_ATTR_TXN_ISOLATION` and `SQLGetInfo` with `SQL_DEFAULT_TXN_ISOLATION` go to psqlODBC, which reads the server's level, `REPEATABLE READ`, at connect.

The wrapper knows that a connection is Aurora DSQL only once it connects. It drops an isolation level set before connecting. For a level other than `REPEATABLE READ`, it logs `[DSQL] SQL_ATTR_TXN_ISOLATION=<name>, set before connect, was changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at REPEATABLE READ.` at WARNING level, where `<name>` is the level asked for, such as `SQL_TXN_SERIALIZABLE`. A connect that would return `SQL_SUCCESS` then returns `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` and the same message without `[DSQL] `. When the wrapper also replaced `SSLmode` (see [TLS](#tls) and [SSL mode changed](./plugins/aurora-dsql-authentication-plugin.md#ssl-mode-changed)), the record holds the TLS message first, then this one. So an application that sets `SQL_TXN_SERIALIZABLE` before connecting can see that it gets `REPEATABLE READ`.

### Catalog Functions

Catalog functions go to psqlODBC unchanged.

Aurora DSQL's primary-key index includes every column of the table, after the key columns. psqlODBC reads that index, so some versions report more key columns than the table has:

| Function                                  | Result on Aurora DSQL                   | Correct from psqlODBC                         |
|-------------------------------------------|-----------------------------------------|-----------------------------------------------|
| `SQLPrimaryKeys`                          | every column of the table               | 18.00.0002                                    |
| `SQLStatistics`                           | every column, for the primary-key index | none: the extra columns are part of the index |
| `SQLSpecialColumns` with `SQL_BEST_ROWID` | no columns                              | 17.00.0006                                    |

A tool that builds `UPDATE` and `DELETE` statements from these key columns then matches rows on every column, which finds no row when a column is NULL or holds an inexact value. Use psqlODBC 18.00.0002 or later, and take the key from `SQLPrimaryKeys`.

`SQLStatistics` also lists an index that `CREATE INDEX ASYNC` is still building, or whose build failed.

## Settings the Wrapper Rejects

The wrapper checks these on every connect, before it loads the underlying driver. A problem fails the connect with `SQL_ERROR` and SQLSTATE `08001`, and the wrapper logs the message at ERROR level. With `RDS_AUTH_TYPE=DSQL`, the connect also fails with `08001` when `SERVER` and `SERVERNAME` name different hosts, or when a cluster ID in `SERVER` or `IAM_HOST` can't be expanded because the region is missing or invalid (see [Cluster IDs](./plugins/aurora-dsql-authentication-plugin.md#cluster-ids)). When a connect has more than one problem, only the first is reported.

### RDS IAM Authentication Types

The `RDS_AUTH_TYPE` values `IAM`, `ADFS`, `OKTA` and `AWS_SSO` sign RDS IAM tokens, which Aurora DSQL rejects. When `SERVER` or `IAM_HOST` is an Aurora DSQL endpoint, or `DATABASE_DIALECT` is `AURORA_DSQL`, the connect fails, for example:

```
RDS_AUTH_TYPE=IAM doesn't work with Aurora DSQL: it signs an RDS IAM token, which Aurora DSQL rejects (SERVER is an Aurora DSQL endpoint). Use RDS_AUTH_TYPE=DSQL, which signs an Aurora DSQL token with the credentials of AWS_PROFILE or the default credential chain; for an AWS IAM Identity Center (SSO) profile, run 'aws sso login' first.
```

With the dialect, the reason in parentheses is `DATABASE_DIALECT=AURORA_DSQL`. For an RDS or Aurora PostgreSQL database, remove `DATABASE_DIALECT`.

Other authentication types are allowed:

- `DATABASE`, or no `RDS_AUTH_TYPE`: `PWD` must be an Aurora DSQL token that you generate.
- `SECRETS_MANAGER`: the secret's password must be a current Aurora DSQL token. The wrapper logs at INFO level: `[DSQL] With RDS_AUTH_TYPE=SECRETS_MANAGER, the secret's password must be a current Aurora DSQL token: DSQL accepts no other password. RDS_AUTH_TYPE=DSQL generates the token instead.`

With either, when neither `DATABASE` nor its short form `DX` is set, the wrapper uses `postgres`, whatever the `SSLmode`. See [TLS](#tls) for the `SSLmode` to use.

### Aurora-Only Plugins

These plugins need Aurora or RDS cluster features that Aurora DSQL doesn't have: `ENABLE_LIMITLESS`, `ENABLE_CLUSTER_FAILOVER`, `ENABLE_SRW_SPLIT`, `ENABLE_RW_SPLIT`, `ENABLE_CUSTOM_ENDPOINT` and `ENABLE_BLUE_GREEN`. When any of them is `1` on an Aurora DSQL connection, the connect fails with a message that lists them all, for example:

```
Aurora DSQL doesn't support these plugins, which need Aurora or RDS cluster features: ENABLE_CLUSTER_FAILOVER, ENABLE_RW_SPLIT. Set them to 0. The driver treats this connection as Aurora DSQL because RDS_AUTH_TYPE=DSQL.
```

The reason at the end is the first that holds of `RDS_AUTH_TYPE=DSQL`, `SERVER is an Aurora DSQL endpoint` and `DATABASE_DIALECT=AURORA_DSQL`. Set the options to `0` rather than removing them: a value in the DSN still applies when the connection string leaves the option out.

`ENABLE_AURORA_INITIAL_CONNECTION_STRATEGY` isn't rejected: that plugin does nothing for a host that isn't an Aurora cluster endpoint.

### A Handle Used for Another Database

A connection handle keeps the kind of database it was first used to connect to. When a handle that was first used to connect to Aurora DSQL connects to a database that isn't Aurora DSQL, or a handle that was first used to connect to another database connects to Aurora DSQL, the connect fails with SQLSTATE `08001`. Allocate a new connection handle for the other database. A later connect on the same handle also keeps the settings of its earlier connects that its connection string doesn't replace, such as a leftover `RDS_AUTH_TYPE=DSQL`, so such a connect can count as Aurora DSQL and fail with another error: allocate a new handle for it too.

## Aurora DSQL Behaviors the Application Handles

### Transactions and DDL

- A transaction can have one DDL statement and no DML. A second DDL statement, or a mix of DDL and DML, fails with SQLSTATE `0A000`. Run each DDL statement in its own transaction, or in autocommit mode ([DDL and distributed transactions](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-ddl.html)).
- Aurora DSQL builds indexes in the background. Write `CREATE INDEX ASYNC`: a plain `CREATE INDEX` fails on a table that has rows. `CREATE INDEX ASYNC` returns a `job_id` at once, and queries use the index once the job completes. To wait for it, run `CALL sys.wait_for_job('<job_id>')`, which returns false when the build failed (psqlODBC returns it as `0`, and `1` when the build finished); then drop the index and create it again ([Asynchronous indexes in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-create-index-async.html)). Send it as a plain `CALL` statement: psqlODBC turns the ODBC escape `{CALL sys.wait_for_job(?)}` into a `SELECT`, which Aurora DSQL rejects for a procedure (SQLSTATE `42809`).
- `TRUNCATE` isn't supported. Use `DELETE FROM <table>` in batches of up to 3,000 rows and 10 MiB, the most one transaction can change: a larger `DELETE` fails with SQLSTATE `54000`, also in autocommit mode. Or drop the table and create it again.
- Temporary tables aren't supported. Use a common table expression (`WITH ...`), or a regular table that the application cleans up ([Migrating from PostgreSQL to Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-postgresql-compatibility-migration-guide.html)).

The wrapper passes these errors through unchanged. For the other limits, such as the 5-minute transaction time, see [Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html).

### Conflicts

Aurora DSQL uses optimistic concurrency control and takes no locks: it checks for conflicts when the transaction commits. When another transaction changed the same rows and committed first, the transaction fails with SQLSTATE `40001`, usually at `COMMIT` (in autocommit mode, at the statement). Roll back and run the whole transaction again. `SELECT ... FOR UPDATE` doesn't make other transactions wait: Aurora DSQL checks the selected rows at commit as if the transaction had written them, so the transaction fails with `40001` when another one changed them and committed first ([Concurrency control in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-concurrency-control.html)).

## Additional Reading

- [Using the AWS Advanced ODBC Wrapper](./using-the-aws-odbc-wrapper.md)
- [Aurora DSQL Authentication Plugin](./plugins/aurora-dsql-authentication-plugin.md)
- [What is Amazon Aurora DSQL?](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html)
- [Migrating from PostgreSQL to Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-postgresql-compatibility-migration-guide.html)
