# Support for Amazon Aurora DSQL

The AWS Advanced ODBC Wrapper supports [Amazon Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html) with [psqlODBC](https://github.com/postgresql-interfaces/psqlodbc) as the underlying driver. Aurora DSQL is PostgreSQL-compatible, but it doesn't support some features that psqlODBC uses: savepoints, cursors, `SET statement_timeout` and `SET SESSION CHARACTERISTICS`. On an Aurora DSQL connection, the wrapper changes psqlODBC settings and answers some ODBC calls itself, so that psqlODBC doesn't send statements Aurora DSQL rejects. It also rejects settings that can't work with Aurora DSQL, and logs hints for some Aurora DSQL limits.

This page lists those changes, and the Aurora DSQL behaviors the application must handle.

## General Usage

Sample connection string, with the Unicode wrapper on Linux:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

`RDS_AUTH_TYPE=DSQL` makes the wrapper sign an Aurora DSQL authentication token for each connection. To retry statements that fail with an optimistic concurrency control (OCC) conflict, see the [OCC Retry Plugin](./plugins/occ-retry-plugin.md).

The warnings and hints on this page go to the wrapper's log, in the `aws-odbc-wrapper` folder of the user's temporary directory (see [Logging](./using-the-aws-odbc-wrapper.md#logging)).

## When the Wrapper Treats a Connection as Aurora DSQL

The wrapper treats a connection as Aurora DSQL when any of these holds (keys and values are case-insensitive):

- `RDS_AUTH_TYPE=DSQL`
- `SERVER` is an Aurora DSQL endpoint, such as `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws`
- `DATABASE_DIALECT=AURORA_DSQL`

Everything on this page applies to such a connection, whatever its authentication type.

- With AWS PrivateLink, a DNS alias or an IP address in `SERVER`, and an authentication type other than `DSQL` (for example `DATABASE`, with a token you generate as `PWD`), set `DATABASE_DIALECT=AURORA_DSQL`. Otherwise the wrapper treats the connection as Aurora PostgreSQL, and nothing on this page applies.
- Don't set `DATABASE_DIALECT=AURORA_DSQL` for an RDS or Aurora PostgreSQL database, for example to avoid topology queries: it turns on everything on this page.
- When `DATABASE_DIALECT` names another dialect on an Aurora DSQL connection, the wrapper uses that dialect, which expects Aurora or RDS topology, but still applies this page. It logs a warning on each connect, for example:

  ```
  [DSQL] DATABASE_DIALECT=AURORA_POSTGRESQL on an Aurora DSQL connection (SERVER is an Aurora DSQL endpoint): the driver uses that dialect, which expects Aurora or RDS topology. Remove DATABASE_DIALECT or set it to AURORA_DSQL.
  ```

## psqlODBC Settings the Wrapper Sets

On every connect, the wrapper sets four psqlODBC options. Its value replaces yours wherever you set it: in the connection string, `BASE_CONN` or the base DSN, in the long or the short form, or in psqlODBC's `CX` flags. When your value differs, the wrapper logs a warning.

| psqlODBC Option                          | Set To  | Why                                                                                               | Warning When Your Value Differs                                                                                                                                  |
|------------------------------------------|---------|---------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Protocol` (short form `A1`)             | `7.4-0` | Other rollback levels make psqlODBC send `SAVEPOINT`, which Aurora DSQL doesn't support.          | `[DSQL] Overriding Protocol=<value> with 7.4-0: Aurora DSQL doesn't support savepoints`                                                                          |
| `IgnoreTimeout` (short form `D9`)        | `1`     | A query timeout makes psqlODBC send `SET statement_timeout`, which Aurora DSQL doesn't support.   | `[DSQL] Overriding IgnoreTimeout=<value> with 1: Aurora DSQL doesn't support statement_timeout`                                                                  |
| `UseDeclareFetch` (short form `B6`)      | `0`     | With `1`, psqlODBC reads results through `DECLARE ... CURSOR`, which Aurora DSQL doesn't support. | `[DSQL] Overriding UseDeclareFetch=<value> with 0: Aurora DSQL doesn't support cursors`                                                                          |
| `UseServerSidePrepare` (short form `C8`) | `0`     | See [Server-Side Prepare](#server-side-prepare).                                                  | `[DSQL] Overriding UseServerSidePrepare=<value> with 0: freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL` |

`<value>` is your value. A value set only in `CX` is replaced without a warning.

psqlODBC's connection attributes `SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE`, `SQL_ATTR_PGOPT_USE_DECLAREFETCH` and `SQL_ATTR_PGOPT_IGNORETIMEOUT` would undo three of these options. On an open Aurora DSQL connection, setting one of them to another value returns `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02`, for example `SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL`, and the option keeps the wrapper's value. A value set before connecting is dropped at connect, with a warning in the log: `[DSQL] Ignoring SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE=<n> set before connect: ...`.

### Savepoints and Errors in a Transaction

With `Protocol=7.4-0`, psqlODBC doesn't roll back a failed statement inside a transaction. After any error in a transaction, call `SQLEndTran` with `SQL_ROLLBACK` before you go on. A `COMMIT` after an error returns success but saves nothing.

`SAVEPOINT`, `RELEASE SAVEPOINT` and `ROLLBACK TO SAVEPOINT` statements that the application sends fail with Aurora DSQL's error.

### Server-Side Prepare

With psqlODBC's default, `UseServerSidePrepare=1`, a prepared statement gets a named plan on the server. When the application frees such a statement, or prepares it again, inside a transaction, psqlODBC sends `DEALLOCATE` for the plan and ignores the result. psqlODBC versions before 18.00.0003 wrap that `DEALLOCATE` in a `SAVEPOINT`, whatever the `Protocol` setting. Aurora DSQL rejects the `SAVEPOINT`, which aborts the transaction, and psqlODBC doesn't report the error: every call, the `COMMIT` included, returns success, but the transaction's changes are lost. psqlODBC 18.00.0003 and later send the `DEALLOCATE` alone at `Protocol=7.4-0`; if Aurora DSQL rejects it, psqlODBC rolls the transaction back without reporting it, and the statements after it commit without the ones before it. A common case is a statement with parameters followed by a commit, such as pyodbc's `connection.execute("UPDATE t SET v = ? WHERE id = ?", 1, 2)` and then `connection.commit()`. In autocommit mode nothing is lost.

With `UseServerSidePrepare=0`, psqlODBC puts the parameter values into the statement text itself, so it creates and frees no plans. This has costs:

- Every `SQLExecute` sends the whole statement, and Aurora DSQL plans it each time.
- `SQLDescribeParam` fails for a parameter that isn't bound yet, with `Unfortunately couldn't get this paramater's info`. pyodbc, for example, calls it only for `None` values, and then uses `SQL_VARCHAR`.

`SQLNumResultCols` and `SQLDescribeCol` before `SQLExecute` still work, and a schema change no longer makes a prepared statement fail with `cached plan must not change result type`.

Keyset-driven cursors (`SQL_CURSOR_KEYSET_DRIVEN`) still make psqlODBC create and free a plan, with the same risk inside a transaction, and they rely on the `ctid` column. Use forward-only or static cursors.

### Large Results

With `UseDeclareFetch=0`, psqlODBC reads a query's whole result into memory when the statement runs. Read large results in pages, for example with `LIMIT` and a `WHERE` condition on the last key read.

## ODBC Calls the Wrapper Answers

On an open Aurora DSQL connection, the wrapper answers some calls itself, so that psqlODBC doesn't send statements Aurora DSQL rejects.

> [!NOTE]\
> On an open connection, `SQLGetDiagField` returns psqlODBC's diagnostics instead of the wrapper's records described in this section, with the Windows driver manager and with unixODBC. Read them with `SQLGetDiagRec` or `SQLError`.

### Query Timeout

| Call                                                                                                                  | Result                                                                                                                                                                                                                                |
|-----------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SQLSetStmtAttr` with `SQL_ATTR_QUERY_TIMEOUT` above `0`                                                              | `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` and `SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it limits each transaction to 5 minutes)`. The timeout stays `0`, which `SQLGetStmtAttr` returns. |
| `SQLSetConnectAttr` with `SQL_ATTR_QUERY_TIMEOUT` above `0`, or ODBC 2 `SQLSetConnectOption` with `SQL_QUERY_TIMEOUT` | The same, on the connection handle.                                                                                                                                                                                                   |
| Either call with `0`                                                                                                  | Passed to psqlODBC.                                                                                                                                                                                                                   |
| ODBC 2 `SQLSetStmtOption` with `SQL_QUERY_TIMEOUT`                                                                    | Passed to psqlODBC, which ignores it because of `IgnoreTimeout=1`. This needs psqlODBC 12.02.0000 or later.                                                                                                                           |

The wrapper logs the message at INFO level, with the value asked for: `[DSQL] <message> (requested <n>)`. Aurora DSQL ends a transaction that runs longer than 5 minutes ([Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html)).

### Transaction Isolation

Aurora DSQL runs every transaction at `REPEATABLE READ`, which is snapshot isolation ([Concurrency control in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-concurrency-control.html)). Snapshot isolation allows write skew: two transactions that read the same rows and each change different ones can both commit. To make them conflict, read the rows that a decision depends on with `SELECT ... FOR UPDATE` (see [Row Locking](#row-locking)).

| Call                                                                                                                                         | Result                                                                                                                                                                                        |
|----------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SQLSetConnectAttr` with `SQL_ATTR_TXN_ISOLATION` set to `SQL_TXN_REPEATABLE_READ`, or ODBC 2 `SQLSetConnectOption` with `SQL_TXN_ISOLATION` | `SQL_SUCCESS`                                                                                                                                                                                 |
| The same with `SQL_TXN_READ_UNCOMMITTED` or `SQL_TXN_READ_COMMITTED`                                                                         | `SQL_SUCCESS_WITH_INFO` with SQLSTATE `01S02` and `SQL_ATTR_TXN_ISOLATION changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at REPEATABLE READ`, logged at WARNING level |
| The same with `SQL_TXN_SERIALIZABLE`                                                                                                         | `SQL_ERROR` with SQLSTATE `HY024` and `SQL_TXN_SERIALIZABLE isn't supported: Aurora DSQL runs every transaction at REPEATABLE READ (snapshot isolation)`, logged at ERROR level               |
| The same with any other value                                                                                                                | `SQL_ERROR` with SQLSTATE `HY024` and `Invalid SQL_ATTR_TXN_ISOLATION value <n>`, logged at ERROR level                                                                                       |
| `SQLGetConnectAttr` with `SQL_ATTR_TXN_ISOLATION`                                                                                            | `SQL_TXN_REPEATABLE_READ`                                                                                                                                                                     |
| `SQLGetInfo` with `SQL_DEFAULT_TXN_ISOLATION` or `SQL_TXN_ISOLATION_OPTION`                                                                  | `SQL_TXN_REPEATABLE_READ`                                                                                                                                                                     |

The wrapper knows that a connection is Aurora DSQL only once it connects. It drops an isolation level set before connecting, and for a level other than `REPEATABLE READ` it logs `[DSQL] Ignoring SQL_ATTR_TXN_ISOLATION=<n> set before connect: Aurora DSQL runs every transaction at REPEATABLE READ` at WARNING level. The connect doesn't return a warning for it, so an application that sets `SQL_TXN_SERIALIZABLE` before connecting gets `REPEATABLE READ` with only that log line.

### Catalog Functions

Catalog functions go to psqlODBC unchanged. `SQLProcedures`, `SQLProcedureColumns` and `SQLColumnPrivileges` can return no rows. `SQLStatistics` also lists an index that `CREATE INDEX ASYNC` is still building, or whose build failed.

## Settings the Wrapper Rejects

The wrapper checks these on every connect, before it loads the underlying driver. A problem fails the connect with `SQL_ERROR` and SQLSTATE `08001`, and the wrapper logs the message at ERROR level. An invalid option value is reported first, with SQLSTATE `01S00`. Then the authentication type is checked, then the plugins, then `ENABLE_OCC_RETRY`, then `MAX_CONNECTION_LIFETIME`; only the first problem found is reported.

### RDS IAM Authentication Types

The `RDS_AUTH_TYPE` values `IAM`, `ADFS`, `OKTA` and `AWS_SSO` sign RDS IAM tokens, which Aurora DSQL rejects. When `SERVER` or `IAM_HOST` is an Aurora DSQL endpoint, the connect fails, for example:

```
RDS_AUTH_TYPE=IAM doesn't work with Aurora DSQL: it signs an RDS IAM token, which Aurora DSQL rejects (SERVER is an Aurora DSQL endpoint). Use RDS_AUTH_TYPE=DSQL, which signs an Aurora DSQL token with the credentials of AWS_PROFILE or the default credential chain; for an AWS IAM Identity Center (SSO) profile, run 'aws sso login' first.
```

Other authentication types are allowed:

- `DATABASE`, or no `RDS_AUTH_TYPE`: `PWD` must be an Aurora DSQL token that you generate.
- `SECRETS_MANAGER`: the secret's password must be a current Aurora DSQL token. The wrapper logs at INFO level: `[DSQL] With RDS_AUTH_TYPE=SECRETS_MANAGER, the secret's password must be a current Aurora DSQL token: DSQL accepts no other password. RDS_AUTH_TYPE=DSQL generates the token instead.`

### Aurora-Only Plugins

These plugins need Aurora or RDS cluster features that Aurora DSQL doesn't have: `ENABLE_LIMITLESS`, `ENABLE_CLUSTER_FAILOVER`, `ENABLE_SRW_SPLIT`, `ENABLE_RW_SPLIT`, `ENABLE_CUSTOM_ENDPOINT` and `ENABLE_BLUE_GREEN`. When any of them is `1` on an Aurora DSQL connection, the connect fails with a message that lists them all, for example:

```
Aurora DSQL doesn't support these plugins, which need Aurora or RDS cluster features: ENABLE_CLUSTER_FAILOVER, ENABLE_RW_SPLIT. Set them to 0. The driver treats this connection as Aurora DSQL because RDS_AUTH_TYPE=DSQL.
```

The reason at the end is the first that holds of `RDS_AUTH_TYPE=DSQL`, `SERVER is an Aurora DSQL endpoint` and `DATABASE_DIALECT=AURORA_DSQL`. Set the options to `0` rather than removing them: a value in the DSN still applies when the connection string leaves the option out.

`ENABLE_AURORA_INITIAL_CONNECTION_STRATEGY` isn't rejected: that plugin does nothing for a host that isn't an Aurora cluster endpoint.

### OCC Retry Outside Aurora DSQL

`ENABLE_OCC_RETRY=1` on a connection that isn't Aurora DSQL fails the connect. See the [OCC Retry Plugin](./plugins/occ-retry-plugin.md#requirements).

### Connection Lifetime Outside Aurora DSQL

`MAX_CONNECTION_LIFETIME` other than `0` on a connection that isn't Aurora DSQL fails the connect, for example:

```
MAX_CONNECTION_LIFETIME=1800 works only with Aurora DSQL. Use an Aurora DSQL endpoint as SERVER, or set RDS_AUTH_TYPE=DSQL or DATABASE_DIALECT=AURORA_DSQL; otherwise set MAX_CONNECTION_LIFETIME=0.
```

## Aurora DSQL Behaviors the Application Handles

### 60-Minute Connection Limit

Aurora DSQL closes every connection after 60 minutes, without an error code or message ([Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html)). The next statement on the connection fails with SQLSTATE `08S01`, and so does every later call. The wrapper doesn't reconnect. Call `SQLDisconnect` and connect again: `SQLDriverConnect` refuses a handle that is still connected.

Recycle pooled connections before they're 55 minutes old, as the Aurora DSQL connectors do. A pool in the application that limits only idle time doesn't prevent this; a driver manager's pool does, because the wrapper retires its connections, as described below.

Once a connection is `MAX_CONNECTION_LIFETIME` seconds old, counted from the start of its connect, `SQLGetConnectAttr` with `SQL_ATTR_CONNECTION_DEAD` returns `SQL_CD_TRUE`, without asking the server. The Windows driver manager and unixODBC read this attribute before they reuse a pooled connection, and replace a connection that reports it with a new one, which gets a new token. The wrapper logs this at INFO level, once per connection:

```
[DSQL] Retiring the connection after 3300 seconds (MAX_CONNECTION_LIFETIME=3300): SQL_ATTR_CONNECTION_DEAD now reports it dead, so a connection pool replaces it.
```

| Option                    | Description                                                                                             | Value                   | Default |
|---------------------------|---------------------------------------------------------------------------------------------------------|-------------------------|---------|
| `MAX_CONNECTION_LIFETIME` | The age in seconds from which `SQL_ATTR_CONNECTION_DEAD` reports a connection dead. `0` turns this off. | `0`, or `120` to `3300` | `3300`  |

A value outside that range fails the connect with SQLSTATE `01S00`, for example `MAX_CONNECTION_LIFETIME=3600 (use 0 for off, or an integer from 120 to 3300)`. The wrapper reads the option on every connect. To turn retirement off, set it to `0`, also on a reused connection handle. On a connection that isn't Aurora DSQL, see [Connection Lifetime Outside Aurora DSQL](#connection-lifetime-outside-aurora-dsql).

Turn on the driver manager's pool, and keep its idle timeout at 600 seconds or less, as the Aurora DSQL connectors do:

- Windows: in ODBC Data Source Administrator, on the Connection Pooling tab, turn on pooling for the wrapper's driver and set its timeout. The application turns pooling on with `SQLSetEnvAttr` and `SQL_ATTR_CONNECTION_POOLING` before it allocates its environment.
- unixODBC: in `odbcinst.ini`, set `CPTimeout` to more than 0 and at most 600 in the wrapper's driver section, and connect with `DRIVER={<that section's name>}` or a DSN whose `Driver` names it. Turn pooling on with `Pooling=Yes` in the `[ODBC]` section, or in the application with `SQL_ATTR_CONNECTION_POOLING` (pyodbc does this by default). `CPTimeToLive` counts how often a connection is reused, not its age.

Outside a driver manager's pool nothing is forced: a statement on an older connection still runs until Aurora DSQL closes it. An application that reads `SQL_ATTR_CONNECTION_DEAD` itself gets `SQL_CD_TRUE` and should disconnect and connect again, as PHP does for its persistent ODBC links (`odbc.check_persistent`, on by default). A pool inside the application, such as SQLAlchemy's, keeps its connections open, so the driver manager never checks them: set that pool's recycle age to 55 minutes or less, for example `pool_recycle=3300`.

When `SQLExecDirect` or `SQLExecute` fails on a connection that is at least 55 minutes old, and psqlODBC reports the connection closed, the wrapper logs this at WARNING level, once per connection:

```
[DSQL] The connection failed after 61 minutes. This may be Aurora DSQL's 60-minute connection limit. Disconnect and connect again, and recycle pooled connections before 55 minutes.
```

The hint says "may" because any network failure after 55 minutes matches too. It isn't logged for SQLSTATE `08P01`, which Aurora DSQL returns for a message larger than 10 MiB, or for failures in `SQLEndTran`, `SQLFetch`, `SQLPrepare`, `SQLParamData` and the catalog functions. The statement's diagnostics aren't changed.

### Advisory Locks

Aurora DSQL doesn't support `pg_advisory_lock` and the related functions. The wrapper doesn't emulate them: reporting success for a lock that wasn't taken would break the application's mutual exclusion. The call fails with the server's error, which the wrapper passes through, and the wrapper logs this at WARNING level:

```
[DSQL] Aurora DSQL doesn't support advisory locks (pg_advisory_lock and related functions), and the driver doesn't emulate them. Remove the lock call, or coordinate through a table row instead.
```

The hint is logged when `SQLExecDirect` or `SQLExecute` fails and the server's message, or the text of an `SQLExecDirect` statement, names a `pg_advisory_` or `pg_try_advisory_` function. Migration tools that take an advisory lock need it turned off.

### Transactions and DDL

- A transaction can have one DDL statement and no DML. A second DDL statement, or a mix of DDL and DML, fails with SQLSTATE `0A000`. Run each DDL statement in its own transaction, or in autocommit mode ([DDL and distributed transactions](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-ddl.html)).
- Aurora DSQL builds indexes in the background. Write `CREATE INDEX ASYNC`: a plain `CREATE INDEX` fails on a table that has rows. `CREATE INDEX ASYNC` returns a `job_id` at once, and queries use the index once the job completes. To wait for it, run `CALL sys.wait_for_job('<job_id>')`, which returns false when the build failed; then drop the index and create it again ([Asynchronous indexes in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-create-index-async.html)).
- `TRUNCATE` isn't supported. Use `DELETE FROM <table>` in batches of up to 3,000 rows and 10 MiB, the most one transaction can change: a larger `DELETE` fails with SQLSTATE `54000`, also in autocommit mode. Or drop the table and create it again.
- Temporary tables aren't supported. Use a common table expression (`WITH ...`), or a regular table that the application cleans up ([Migrating from PostgreSQL to Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-postgresql-compatibility-migration-guide.html)).

The wrapper passes these errors through unchanged. For the other limits, such as the 5-minute transaction time, see [Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html).

### OCC Conflicts

Aurora DSQL reports a conflict between transactions with SQLSTATE `40001`. Retry the whole transaction, as in [Retrying Transactions](./plugins/occ-retry-plugin.md#retrying-transactions). The [OCC Retry Plugin](./plugins/occ-retry-plugin.md) can retry single statements in autocommit mode.

### Row Locking

`SELECT ... FOR UPDATE` takes no lock and doesn't make other transactions wait. Aurora DSQL checks the selected rows at commit as if the transaction had written them: if another transaction changes one of them and commits first, this transaction fails with SQLSTATE `40001` and `(OC000)` ([Concurrency control in Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-concurrency-control.html)). Retry it as for any OCC conflict. `SELECT ... FOR KEY SHARE` works the same way for the rows' key columns: a concurrent delete or key change conflicts with it. Aurora DSQL doesn't support `FOR SHARE` or `FOR NO KEY UPDATE`.

### Schemas for Custom Database Roles

In Aurora DSQL, the `admin` role manages objects in the `public` schema, and other database roles create objects in schemas that `admin` creates and grants to them ([Migrating from PostgreSQL to Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-postgresql-compatibility-migration-guide.html)). For example, as `admin`, after creating the role `app_user` and linking it to an IAM identity ([Using database roles and IAM authentication](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/using-database-and-iam-roles.html)):

```sql
CREATE SCHEMA app;
GRANT USAGE, CREATE ON SCHEMA app TO app_user;
```

`app_user` can then create its tables in `app`. To give it access to tables that `admin` creates there, run `GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA app TO app_user;` after creating them. Run each statement on its own, in autocommit mode. Connections that are already open can take up to an hour to see a new grant; connect again to use it at once.

Then qualify table names with the schema (`app.orders`), or set the search path when the connection opens, with psqlODBC's `ConnSettings` option, which runs SQL right after connecting:

```
ConnSettings={SET search_path TO app};
```

Give one statement only: the wrapper ends an option's value at the first `;`, also inside braces.

## Additional Reading

- [Using the AWS Advanced ODBC Wrapper](./using-the-aws-odbc-wrapper.md)
- [OCC Retry Plugin](./plugins/occ-retry-plugin.md)
- [What is Amazon Aurora DSQL?](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html)
- [Migrating from PostgreSQL to Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/working-with-postgresql-compatibility-migration-guide.html)
