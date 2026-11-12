# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Aurora DSQL support: the [Aurora DSQL Authentication Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/aurora-dsql-authentication-plugin.md) (`RDS_AUTH_TYPE=DSQL`: a token per connection, `verify-full` TLS, cluster IDs), the Aurora DSQL dialect, and the psqlODBC settings, answered ODBC calls and connect-time checks described in [Support for Amazon Aurora DSQL](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/SupportForAuroraDSQL.md)
- [OCC Retry Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/occ-retry-plugin.md) (`ENABLE_OCC_RETRY`): retries an autocommit statement that fails with an Aurora DSQL OCC conflict; an `SQLCancel` from another thread ends its wait
- `MAX_CONNECTION_LIFETIME`, off unless set: once an Aurora DSQL connection is that old, `SQL_ATTR_CONNECTION_DEAD` reports it dead, so a driver manager's pool replaces it; set it to `3300` for a driver manager's pool, and not for applications built on nanodbc
- The Windows DSN setup dialog: the Aurora DSQL authentication mode and dialect, and an Aurora DSQL tab

### Changed

- The `AWS_PROFILE` key restricts Aurora DSQL credentials to that profile: a profile that isn't in the AWS config or credentials file fails the connect with `28000` instead of falling back to environment, container or instance credentials, and the `AWS_PROFILE` environment variable keeps the default credential chain
- RDS IAM authentication types with `DATABASE_DIALECT=AURORA_DSQL` fail at connect; on RDS PostgreSQL, remove `DATABASE_DIALECT`
- On an Aurora DSQL connection with an authentication type other than `DSQL`, an `SSLmode` that psqlODBC would run as `disable` becomes `verify-full` (a value you set is reported with `01S02`), or, for a full mode name in another case such as `Require`, that mode in lowercase; a mode psqlODBC runs is left exactly as set; either way, a missing `DATABASE` becomes `postgres`
- With `RDS_AUTH_TYPE=DSQL`, `SERVERNAME` is read as `SERVER`, so psqlODBC connects to the host the token was signed for, and `SERVER` and `SERVERNAME` naming different hosts fail with `08001`
- On Aurora DSQL the wrapper sets psqlODBC's `UpdatableCursors=0`: keyset-driven and updatable cursors become static and read-only (`01S02`) instead of failing
- On Aurora DSQL, `SQLEndTran` with `SQL_COMMIT` after a failed statement rolls back a transaction that Aurora DSQL ended and returns `25S03`, instead of reporting success for a commit that saved nothing; so does a change of `SQL_ATTR_AUTOCOMMIT`, which commits first and returns that commit's error with autocommit unchanged
- After a connect with the base dialect, which the wrapper uses when neither `DATABASE_DIALECT` nor the underlying driver's name gives the database, the wrapper no longer runs a topology query

### Fixed

- Debug builds no longer log the generated IAM authentication token [(PR #167)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/167)
- `SQLGetInfo(SQL_DRIVER_VER)` now reports the real wrapper version instead of a hardcoded `1.0.0`
- Discovery of the `odbcinst`/`iodbcinst` library on macOS and Linux, which ignored the configured `ODBC_ROOT`
- The ANSI static library is now compiled with `SQL_NOUNICODEMAP`, matching the ANSI shared library
- `SQLCancel` and `SQLCancelHandle` called from another thread no longer wait for the statement's running `SQLExecDirect` or `SQLExecute` to finish. A call canceled before its statement reaches the underlying driver returns `HY008` without running it, and during the underlying driver's execute the cancel goes to the underlying driver at once. Waits inside failover's reconnect and a Blue/Green switchover aren't interrupted: the call returns its own result
- Freeing a statement, disconnecting, or freeing a connection or environment while another thread's `SQLCancel` is still in the underlying driver no longer crashes: the wrapper frees the underlying driver's handles when the cancel returns, and after an `SQLCancel` reaches the underlying driver, the wrapper stays loaded until the process exits
- `SQLDisconnect` frees the descriptors the application allocated on the connection before the underlying driver disconnects: psqlODBC before 18.00.0002 freed them itself, and the wrapper's later free crashed
- `SQLConnect` with an empty user name or password keeps the DSN's `UID` and `PWD`, as psqlODBC does; with `RDS_AUTH_TYPE=DSQL` and no `UID` in the DSN either, it connects as `admin`
- The Windows DSN setup dialog labels the token expiration in seconds, the unit the wrapper reads

## [1.3.1] - 2026-09-08

### Fixed

- Resolution of logical driver names in `odbcinst.ini`, preventing failures to load the underlying driver [(PR #160)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/160)
- Validation of numeric connection attribute parsing [(PR #162)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/162)
- IAM token generation now signs with the database region instead of the SSO region for cross-region AWS IAM Identity Center (SSO) authentication [(PR #168)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/168)

## [1.3.0] - 2026-07-30

### Added

- [AWS IAM Identity Center (SSO) Authentication Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/aws-sso-authentication-plugin.md) [(PR #151)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/151)
- [Browser-Based SAML (Passwordless) Mode for the Okta Authentication Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/okta-authentication-plugin.md#additional-okta-setup-for-browser-mode-passwordless) [(PR #154)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/154)
- Support for selecting a named AWS profile with the [AWS_PROFILE connection option](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/iam-authentication-plugin.md) [(PR #150)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/150)

### Fixed

- Segmentation faults from invalid ODBC handles and monitor thread races [(PR #157)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/157)
- Schema-qualified internal SQL queries for Blue/Green detection [(PR #158)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/158)

## [1.2.0] - 2026-06-16

### Added

- Support for [Aurora MySQL](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/using-the-aws-odbc-wrapper.md#aurora-mysql)
- Support for [Amazon RDS Multi-AZ DB Clusters](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/SupportForRDSMultiAzDBCluster.md) [(PR #141)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/141)
- [Read/Write Splitting Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/read-write-splitting-plugin.md) and [Simple Read/Write Splitting Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/simple-read-write-splitting-plugin.md) [(PR #107)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/107)
- [Blue/Green Deployment Support](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/blue-green-plugin.md) [(PR #101)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/101)
- [Aurora Initial Connection Strategy Plugin](https://github.com/aws/aws-advanced-odbc-wrapper/blob/main/docs/using-the-aws-odbc-wrapper/plugins/aurora-initial-connection-strategy-plugin.md) [(PR #100)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/100)
- MacOS Releases Signed and Notarized [(PR #96)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/96)
- [Flag to Return DSN-Name Only](https://github.com/aws/aws-advanced-odbc-wrapper/pull/142) in SQLDriverConnect [(PR #142)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/142)

### Fixed

- Proper lifetime of the monitor DBC handle [(PR #133)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/133)
- Cluster topology monitor incorrectly running plugins [(PR #130)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/130)
- Improved SQLWCHAR encoding to support iODBC and unixODBC  [(PR #94)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/94)

## [1.1.0] - 2026-03-09

### Added

- 2FA for Okta Authentication Plugin [(PR #72)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/72)
- Ability to specify the username and password keys for Secret Manager Plugin [(PR #84)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/84)
- AWS Custom Endpoint Plugin [(PR #82)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/82)

### Fixed

- Aliased keywords for Server, Username, Password from base DSNs preventing authentication plugins from functioning properly [(PR #76)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/76)
- ODBC Data Source Administrator GUI Deadlocks when testing connections [(PR #83)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/83)
- Incorrect deletions of Limitless and cluster topology monitors [(PR #81)](https://github.com/aws/aws-advanced-odbc-wrapper/pull/81)

## [1.0.0] - 2025-11-25

The Amazon Web Services (AWS) Advanced ODBC Wrapper allows an application to take advantage of AWS authentication, failover feature of the clustered Aurora databases, and provides support for Aurora Limitless databases.

### Added

- Support for PostgreSQL
- AWS IAM Authentication Connection Plugin
- AWS Secrets Manager Authentication Connection Plugin
- AWS Federated Authentication with OKTA and ADFS Connection Plugins
- Limitless Connection Plugin
- Failover Connection Plugin

[1.3.1]: https://github.com/aws/aws-advanced-odbc-wrapper/compare/1.3.0...1.3.1
[1.3.0]: https://github.com/aws/aws-advanced-odbc-wrapper/compare/1.2.0...1.3.0
[1.2.0]: https://github.com/aws/aws-advanced-odbc-wrapper/compare/1.1.0...1.2.0
[1.1.0]: https://github.com/aws/aws-advanced-odbc-wrapper/compare/1.0.0...1.1.0
[1.0.0]: https://github.com/aws/aws-advanced-odbc-wrapper/releases/tag/v1.0.0
