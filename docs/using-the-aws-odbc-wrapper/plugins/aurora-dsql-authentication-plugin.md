## Aurora DSQL Authentication Plugin for the AWS Advanced ODBC Wrapper

[Amazon Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html) authenticates connections with IAM authentication tokens instead of passwords. With `RDS_AUTH_TYPE=DSQL`, the plugin signs a new token for each connection and passes it to the underlying driver as the password. It also applies the defaults Aurora DSQL needs: TLS with certificate verification, the `admin` role and the `postgres` database.

The plugin works with [psqlODBC](https://github.com/postgresql-interfaces/psqlodbc) as the underlying driver: it sets psqlODBC's `SSLmode` and `pqopt` options.

### Prerequisites

1. An Aurora DSQL cluster, and its endpoint (`<cluster-id>.dsql.<region>.on.aws`) or its cluster ID.
2. AWS credentials allowed to connect ([Connecting to your cluster using IAM](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/authentication-authorization.html#authentication-authorization-iam-role-connect)): `dsql:DbConnectAdmin` for the `admin` role, `dsql:DbConnect` for a custom database role.
3. For a custom database role, the role linked to your IAM identity with `AWS IAM GRANT` ([Using database roles and IAM authentication](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/using-database-and-iam-roles.html)).
4. psqlODBC with libpq 16 or later, or a root certificate file. See [Root Certificates](#root-certificates).

### Connection String / DSN Configuration for Aurora DSQL Authentication Plugin Support

| Field               | Connection Option Key | Value                                                                                                                                                                                                  | Default Value     | Sample Value                                       |
|---------------------|-----------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------|----------------------------------------------------|
| Authentication Type | `RDS_AUTH_TYPE`       | Must be `DSQL`.                                                                                                                                                                                        | `database`        | `DSQL`                                             |
| Server              | `SERVER`              | The cluster endpoint, or the cluster ID. See [Cluster IDs](#cluster-ids).                                                                                                                              | nil               | `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws` |
| Port                | `PORT`                | Port of the cluster.                                                                                                                                                                                   | `5432`            | `5432`                                             |
| User Name           | `UID`                 | The database role to connect as. `USER` or `USERNAME` is used when `UID` isn't set. See [Database Roles](#database-roles).                                                                             | `admin`           | `app_user`                                         |
| Database            | `DATABASE`            | Database to connect to.                                                                                                                                                                                | `postgres`        | `postgres`                                         |
| Region              | `REGION`              | The cluster's region, used to sign the token. Needed only when no other source has it. See [Region](#region).                                                                                          | derived           | `us-east-1`                                        |
| IAM Host            | `IAM_HOST`            | The cluster endpoint or cluster ID the token is signed for. Needed only when `SERVER` isn't the cluster endpoint, as with [AWS PrivateLink without private DNS](#aws-privatelink-without-private-dns). | `SERVER`          | `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws` |
| AWS Profile         | `AWS_PROFILE`         | AWS profile to take credentials from. See [AWS Credentials](#aws-credentials).                                                                                                                         | nil               | `dev`                                              |
| Token Expiration    | `TOKEN_EXPIRATION`    | Token lifetime in seconds. `0` means the default. See [Token Expiration](#token-expiration).                                                                                                           | `900`             | `900`                                              |
| SSL Mode            | `SSLmode`             | psqlODBC's TLS mode: `verify-full`, `verify-ca` or `require`. Other values are replaced with `verify-full`. See [TLS](#tls).                                                                           | `verify-full`     | `verify-ca`                                        |
| libpq Options       | `pqopt`               | psqlODBC's libpq options, in braces. The plugin adds `sslrootcert=system` (with `verify-full`) and `application_name=aurora-dsql-odbc/<version>` unless you set them. See [TLS](#tls).                 | set by the plugin | `{sslrootcert=/etc/ssl/certs/ca-certificates.crt}` |
| Database Dialect    | `DATABASE_DIALECT`    | Leave unset, or set `AURORA_DSQL`. See [Database Dialect](#database-dialect).                                                                                                                          | detected          | `AURORA_DSQL`                                      |
| Extra URL Encode    | `EXTRA_URL_ENCODE`    | Not needed: Aurora DSQL tokens connect with `0` or `1`.                                                                                                                                                | `0`               | `0`                                                |

`PWD` is ignored: the token replaces it. The plugin also reads psqlODBC's short forms of `SSLmode` and `pqopt`, `CA` and `D5`.

### Minimal Connection String

> [!NOTE]\
> By default the plugin verifies the server with the system's trusted root certificates (`sslrootcert=system`), which needs libpq 16 or later. On Linux, psqlODBC packages use the system's libpq; check its version with `dpkg -s libpq5` (Debian, Ubuntu) or `rpm -q libpq` (RHEL, Fedora, Amazon Linux).

With libpq 16 or later, on Linux, with the Unicode wrapper:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

With libpq 15 or earlier, also give a root certificate file in `pqopt`. Most Linux systems include Amazon Root CA 1 in their CA bundle: `/etc/ssl/certs/ca-certificates.crt` on Debian and Ubuntu, `/etc/pki/tls/certs/ca-bundle.crt` on RHEL, Fedora and Amazon Linux. This string works with any libpq version:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;pqopt={sslrootcert=/etc/ssl/certs/ca-certificates.crt};
```

Both connect as `admin` to `postgres` with `SSLmode=verify-full`. The region comes from the endpoint, and the credentials from the default AWS credential chain. Use psqlODBC's ANSI driver (`psqlodbca.so`) with the ANSI wrapper. Driver paths vary by system. For Windows, see [Windows](#windows).

### Sample DSN Configuration

#### Linux

In `odbc.ini`, with the wrapper registered in `odbcinst.ini` as in [Getting Started](../../getting-started.md#odbcinstini). On macOS, use the paths of your psqlODBC and wrapper builds and of your root certificate file.

```bash
[ODBC Data Sources]
aurora-dsql     = AWS Advanced ODBC Wrapper Unicode
aurora-dsql-app = AWS Advanced ODBC Wrapper Unicode

# libpq 16 or later (see Minimal Connection String)
[aurora-dsql]
Driver          = AWS Advanced ODBC Wrapper Unicode
SERVER          = abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws
RDS_AUTH_TYPE   = DSQL
BASE_DRIVER     = /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so

# Any libpq version: a cluster ID, a custom database role, an AWS profile and the CA bundle of Debian or Ubuntu
[aurora-dsql-app]
Driver          = AWS Advanced ODBC Wrapper Unicode
SERVER          = abcdefghijklmnopqrstuvwxyz
REGION          = us-east-1
RDS_AUTH_TYPE   = DSQL
UID             = app_user
AWS_PROFILE     = dev
pqopt           = {sslrootcert=/etc/ssl/certs/ca-certificates.crt}
BASE_DRIVER     = /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
```

#### Windows

In ODBC Data Source Administrator (64-bit), add a DSN for `AWS Advanced ODBC Wrapper Unicode` or `AWS Advanced ODBC Wrapper Ansi`:

1. **Data Source Name**: a name for the DSN.
2. **Server**: the cluster endpoint or cluster ID.
3. **Username**: leave it blank to connect as `admin`, or enter a custom database role. Leave **Password**, **Port** and **Database** blank.
4. **Base Driver Location**: psqlODBC's driver DLL, `psqlodbc35w.dll` for the Unicode wrapper or `psqlodbc30a.dll` for the ANSI wrapper, in psqlODBC's `bin` folder. Or set **Base Driver Data Source Name** to a psqlODBC DSN, and set that DSN's SSL Mode to `verify-full`: psqlODBC saves `disable` by default, which the plugin replaces, with a warning, on every connect (see [TLS](#tls)).
5. **Base Driver Connection String**: `pqopt={sslrootcert=C:/certs/AmazonRootCA1.pem}`, with forward slashes (see [Root Certificates](#root-certificates)). Separate other psqlODBC options with `;`, for example `SSLmode=verify-ca;pqopt={sslrootcert=C:/certs/AmazonRootCA1.pem}`.
6. **AWS Profile**: optional. See [AWS Credentials](#aws-credentials).
7. **Database Dialect**: leave it blank, or choose **Aurora DSQL**.
8. On the **Authentication** tab, choose **Aurora DSQL** as the **Authentication Mode**. The tab then shows **AWS Region**, **Expire Time** and **IAM Host**. Set **AWS Region** when **Server** is a cluster ID and no environment variable or AWS profile gives the region (see [Region](#region)). **Expire Time** is in seconds; leave it blank for 900. Leave **IAM Host** blank unless you use [AWS PrivateLink without private DNS](#aws-privatelink-without-private-dns).
9. Choose **Test**, then **Save** and **OK**.

Write `RDS_AUTH_TYPE=DSQL` in upper case in a DSN you'll open in the dialog, for example one created with `Add-OdbcDsn`: the dialog doesn't recognize a lowercase value, and saving the DSN from it then clears the value.

### Region

The token is signed for the cluster's region. The plugin uses the first region it finds:

1. `REGION`
2. The region in `IAM_HOST`, if it's a cluster endpoint
3. The region in `SERVER`, if it's a cluster endpoint
4. The `AWS_REGION` environment variable, then `AWS_DEFAULT_REGION`
5. The `region` in the AWS config file (`~/.aws/config`, or the file `AWS_CONFIG_FILE` names) of the profile named by `AWS_PROFILE`, else by the `AWS_PROFILE` environment variable, else `default`

The region is used in lowercase. There's no default region, and the region isn't read from EC2 instance metadata. Aurora DSQL rejects a token signed for another region (see [Troubleshooting](#troubleshooting)).

### Cluster IDs

With `RDS_AUTH_TYPE=DSQL`, `SERVER` can hold the cluster ID instead of the endpoint, for example `SERVER=abcdefghijklmnopqrstuvwxyz;REGION=us-east-1`.

- A value of exactly 26 letters or digits, in any case, is a cluster ID. Before connecting, the plugin replaces it with `<cluster-id>.dsql.<region>.on.aws` in lowercase, with the region from [Region](#region).
- `IAM_HOST` is expanded the same way.
- Any other value is used as typed: an endpoint, an IP address, `localhost`, or a name with a dot, hyphen or underscore, or of another length.
- Only `RDS_AUTH_TYPE=DSQL` expands cluster IDs. With another authentication type, a 26-character `SERVER` is a host name.
- The Failover plugin's `CLUSTER_ID` option is unrelated. Put the Aurora DSQL cluster ID in `SERVER`.
- The wrapper logs the endpoint at INFO level: `[DSQL] Expanded the cluster ID in SERVER to <endpoint>` (or `IAM_HOST`).
- If no region is found, the connect fails with `DSQL Authentication failed. Region is required when host is a cluster ID (set 'REGION')` (SQLSTATE `08001`).

### Database Roles

The token type depends on `UID`:

| `UID`               | Token                            | IAM action needed     |
|---------------------|----------------------------------|-----------------------|
| not set, or `admin` | admin token                      | `dsql:DbConnectAdmin` |
| any other role name | token for a custom database role | `dsql:DbConnect`      |

Only the exact lowercase `admin` gets an admin token; `UID=Admin` gets a custom-role token, which Aurora DSQL rejects. A custom role must be linked to your IAM identity with `AWS IAM GRANT`.

### Token Expiration

`TOKEN_EXPIRATION` sets the token's lifetime in seconds. The default is `900` (15 minutes), and `0` also means 900. A negative value, or one that isn't a whole number, fails the connect with `Invalid value specified for connection string attribute:` followed by `TOKEN_EXPIRATION` (SQLSTATE `01S00`).

The wrapper sets no maximum. Aurora DSQL documents a maximum of 604,800 seconds (one week) in [Generating an authentication token](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/SECTION_authentication-token.html). A token valid for longer fails when the connection opens.

Aurora DSQL checks the token only when a connection opens, and the plugin signs a new token for every connection, so the default is enough.

### TLS

Aurora DSQL accepts only TLS connections. The plugin sets psqlODBC's `SSLmode` before connecting:

| `SSLmode` you set                                                                                                                                 | Mode used     | Warning logged                                                                   |
|---------------------------------------------------------------------------------------------------------------------------------------------------|---------------|----------------------------------------------------------------------------------|
| not set, or `verify-full`                                                                                                                         | `verify-full` | none                                                                             |
| `verify-ca`                                                                                                                                       | `verify-ca`   | `[DSQL] SSLmode=verify-ca verifies the server certificate but not its host name` |
| `require`                                                                                                                                         | `require`     | `[DSQL] SSLmode=require encrypts but doesn't verify the server certificate`      |
| any other value: `disable`, `allow`, `prefer`, a typo, or psqlODBC's short forms (`r`, `vc`, `vf` and the others), even those meaning a kept mode | `verify-full` | `[DSQL] Overriding SSLmode=<value> with verify-full`                             |

The mode names match in any case. See [Logging](../using-the-aws-odbc-wrapper.md#logging) for where the log is.

When the plugin replaces a mode, the connect returns `SQL_SUCCESS_WITH_INFO` with a `01S02` diagnostic record: `SSLmode=<value> was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, verify-ca or require.` `SQLGetDiagField` reads this record once the wrapper's diagnostics fix in the companion fixes pull request is in; until then, read it with `SQLGetDiagRec` or `SQLError`. If psqlODBC returns its own warning on the same connect, for example `01004` when the application's output connection string buffer is too small, psqlODBC's record is kept and the replacement is only logged.

#### Root Certificates

With `verify-full`, the plugin adds `sslrootcert=system` to `pqopt` unless you set `sslrootcert`. libpq then verifies the server against the system's trusted root certificates, which include Amazon Root CA 1 on most systems ([Configuring SSL/TLS certificates](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/configure-root-certificates.html)). `sslrootcert=system` needs libpq 16 or later.

Set a root certificate file instead:

- with libpq 15 or earlier, which looks for a file named `system`;
- on Windows, where `sslrootcert=system` uses OpenSSL's default certificate paths rather than the Windows certificate store, and can fail with `certificate verify failed` or `SSL error: unregistered scheme`;
- with `verify-ca`: libpq accepts `sslrootcert=system` only with `verify-full`, so the plugin doesn't add it.

On Linux, the system's CA bundle usually includes Amazon Root CA 1: `pqopt={sslrootcert=/etc/ssl/certs/ca-certificates.crt}` on Debian and Ubuntu, `pqopt={sslrootcert=/etc/pki/tls/certs/ca-bundle.crt}` on RHEL, Fedora and Amazon Linux. Otherwise, download [AmazonRootCA1.pem](https://www.amazontrust.com/repository/AmazonRootCA1.pem) and set `pqopt={sslrootcert=/path/to/AmazonRootCA1.pem}`.

> [!WARNING]\
> On Windows, write the path with forward slashes: `pqopt={sslrootcert=C:/certs/AmazonRootCA1.pem}`. libpq reads a backslash in `pqopt` as an escape, so `C:\certs\AmazonRootCA1.pem` would become `C:certsAmazonRootCA1.pem`. Put a path that contains spaces in single quotes: `pqopt={sslrootcert='C:/Program Files/certs/AmazonRootCA1.pem'}`.

The plugin keeps your other `pqopt` options. It also adds `application_name=aurora-dsql-odbc/<version>` unless you set `application_name`.

### AWS Credentials

The plugin signs tokens with credentials from the AWS SDK for C++:

- Without `AWS_PROFILE`, it uses the SDK's default credential chain: environment variables (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`), the profile named by the `AWS_PROFILE` environment variable (else `default`) in `~/.aws/credentials` and `~/.aws/config`, web identity, AWS IAM Identity Center (SSO), then container credentials when `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` or `AWS_CONTAINER_CREDENTIALS_FULL_URI` is set, otherwise the EC2 instance role.
- With `AWS_PROFILE`, the chain reads that profile instead. Any profile type works: access keys, AWS IAM Identity Center (SSO), assume role (`source_profile` and `role_arn`) and `credential_process`. Credentials in environment variables still come first.

The plugin doesn't open a browser for AWS IAM Identity Center (SSO). Sign in with the AWS CLI first, `aws sso login --profile <profile>`, and again when the session expires. Without usable credentials the connect fails before a token is signed; see [Troubleshooting](#troubleshooting).

### Database Dialect

The wrapper picks the Aurora DSQL dialect when `RDS_AUTH_TYPE=DSQL` or the host is a cluster endpoint, so `DATABASE_DIALECT` can stay unset. If you set it, use `AURORA_DSQL` (**Aurora DSQL** in the Windows dialog). With another dialect, such as `AURORA_POSTGRESQL`, the wrapper looks for an Aurora cluster topology that Aurora DSQL doesn't have, and every connect takes several seconds longer.

### Connection Pooling

The plugin signs a new token for each new connection; it doesn't cache tokens. An open connection stays open after its token expires.

Aurora DSQL closes every connection after 60 minutes ([Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html)), and the wrapper doesn't reopen it. Set your connection pool's maximum connection lifetime under 55 minutes, so the pool replaces connections before Aurora DSQL closes them. Windows driver-manager connection pooling has no such setting: its `CPTimeout` closes only connections that stay unused, so a pooled connection in steady use stays open until Aurora DSQL closes it.

### AWS PrivateLink Without Private DNS

Without private DNS, `SERVER` is the VPC endpoint's DNS name ([Managing clusters using AWS PrivateLink](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/privatelink-managing-clusters.html)). The token must still be signed for the cluster endpoint, and `verify-full` fails because the server certificate doesn't name the VPC endpoint. Set `IAM_HOST` to the cluster endpoint or cluster ID, `SSLmode` to `verify-ca` with a root certificate file, and the cluster ID in libpq's `options`:

```
SERVER=vpce-0123456789abcdef0-abcdefgh.dsql-fnh4.us-east-1.vpce.amazonaws.com;IAM_HOST=abcdefghijklmnopqrstuvwxyz;REGION=us-east-1;RDS_AUTH_TYPE=DSQL;SSLmode=verify-ca;pqopt={sslrootcert=/path/to/AmazonRootCA1.pem options='-c amzn-cluster-id=abcdefghijklmnopqrstuvwxyz'};BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

### Troubleshooting

The wrapper's log has more detail; see [Logging](../using-the-aws-odbc-wrapper.md#logging).

#### Region is required for a cluster ID

`DSQL Authentication failed. Region is required when host is a cluster ID (set 'REGION')` (SQLSTATE `08001`)

`SERVER` or `IAM_HOST` holds a cluster ID and no region was found. Set `REGION` or `AWS_REGION`, add a `region` to the AWS profile, or use the cluster endpoint.

#### Missing region for a host name

`DSQL Authentication failed. Missing required parameters: Region (set 'REGION')` (SQLSTATE `08001`)

`SERVER` isn't a cluster endpoint, for example a DNS alias or a VPC endpoint, and no other source has the region. Set `REGION`, or set `IAM_HOST` to the cluster endpoint.

#### No AWS credentials

`Unable to resolve AWS credentials for DSQL authentication with the 'AWS_PROFILE' profile '<profile>'. If it is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile <profile>'.`

The profile's credentials are missing or expired. For an AWS IAM Identity Center (SSO) profile, run the `aws sso login` command shown, then connect again. Otherwise check the profile name and its entries in `~/.aws/config` and `~/.aws/credentials`.

`Unable to resolve AWS credentials for DSQL authentication from the default credential chain (such as environment variables, ~/.aws/credentials, an AWS IAM Identity Center (SSO) session, or a container or instance role)`

No profile is named, and the default chain found no credentials. Set `AWS_PROFILE`, or provide credentials through one of the chain's sources.

Both fail the connect with `SQL_ERROR` before a token is signed.

#### SSL mode changed

`SSLmode=<value> was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, verify-ca or require.` (SQLSTATE `01S02`)

A warning: the connect returned `SQL_SUCCESS_WITH_INFO`, and the connection is open with `verify-full`. Set `SSLmode` to one of the three modes, or remove it. The value can come from the connection string, the DSN, or a psqlODBC DSN set as the base DSN.

#### Root certificate errors

With `sslrootcert=system`, the connect fails with a message that contains `root certificate file "system" does not exist`, `certificate verify failed` or `SSL error: unregistered scheme`, and the log has:

`[DSQL] The server certificate couldn't be verified with sslrootcert=system, which needs libpq 16 or later and, on Windows, OpenSSL's default certificate paths. Set pqopt={sslrootcert=<path to AmazonRootCA1.pem>} instead.`

Set `sslrootcert` to a root certificate file, as in [Root Certificates](#root-certificates).

#### Access denied

`unable to accept connection, access denied` (SQLSTATE `08001`)

Aurora DSQL rejected the token. Check that:

- the credentials allow `dsql:DbConnectAdmin` (for `admin`) or `dsql:DbConnect` (for other roles) on the cluster;
- a custom role is linked to the IAM identity with `AWS IAM GRANT`, and `UID` is the role's name;
- the region is the cluster's region (for a token signed for another region, the message continues with `Credential should be scoped to a valid region.`);
- `IAM_HOST`, if set, is the cluster endpoint or cluster ID.

#### Invalid token expiration

`Invalid value specified for connection string attribute:` followed by `TOKEN_EXPIRATION` (SQLSTATE `01S00`)

`TOKEN_EXPIRATION` must be a whole number of seconds, `0` or more. A value above 604,800 passes this check but fails when the connection opens (see [Token Expiration](#token-expiration)).
