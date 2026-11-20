## Aurora DSQL Authentication Plugin for the AWS Advanced ODBC Wrapper

[Amazon Aurora DSQL](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/what-is-aurora-dsql.html) authenticates connections with IAM authentication tokens instead of passwords. With `RDS_AUTH_TYPE=DSQL`, the plugin signs a new token for each connection and passes it to the underlying driver as the password. It also applies the defaults Aurora DSQL needs: TLS with certificate verification, the `admin` role and the `postgres` database.

The plugin works with [psqlODBC](https://github.com/postgresql-interfaces/psqlodbc) as the underlying driver: it sets psqlODBC's `SSLmode` and `pqopt` options.

### Prerequisites

1. An Aurora DSQL cluster, and its endpoint (`<cluster-id>.dsql.<region>.on.aws`).
2. AWS credentials allowed to connect ([Connecting to your cluster using IAM](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/authentication-authorization.html#authentication-authorization-iam-role-connect)): `dsql:DbConnectAdmin` for the `admin` role, `dsql:DbConnect` for a custom database role.
3. For a custom database role, the role linked to your IAM identity with `AWS IAM GRANT` ([Using database roles and IAM authentication](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/using-database-and-iam-roles.html)).
4. psqlODBC with libpq 16 or later, a system CA bundle, or a root certificate file. See [Root Certificates](#root-certificates).

### Connection String / DSN Configuration for Aurora DSQL Authentication Plugin Support

| Field               | Connection Option Key | Value                                                                                                                                                                                                  | Default Value     | Sample Value                                       |
|---------------------|-----------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------|----------------------------------------------------|
| Authentication Type | `RDS_AUTH_TYPE`       | Must be `DSQL`.                                                                                                                                                                                        | `database`        | `DSQL`                                             |
| Server              | `SERVER`              | The cluster endpoint. `SERVERNAME` is read as `SERVER` when `SERVER` isn't set.                                                                                                                        | nil               | `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws` |
| Port                | `PORT`                | Port of the cluster.                                                                                                                                                                                   | `5432`            | `5432`                                             |
| User Name           | `UID`                 | The database role to connect as. `USER` or `USERNAME` is used when `UID` isn't set. See [Database Roles](#database-roles).                                                                             | `admin`           | `app_user`                                         |
| Database            | `DATABASE`            | Database to connect to.                                                                                                                                                                                | `postgres`        | `postgres`                                         |
| Region              | `REGION`              | The cluster's region, used to sign the token. Needed only when no other source has it. See [Region](#region).                                                                                          | derived           | `us-east-1`                                        |
| IAM Host            | `IAM_HOST`            | The cluster endpoint the token is signed for. Needed only when `SERVER` isn't the cluster endpoint, as with [AWS PrivateLink without private DNS](#aws-privatelink-without-private-dns).               | `SERVER`          | `abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws` |
| AWS Profile         | `AWS_PROFILE`         | AWS profile to take credentials from. With this key only that profile is used. See [AWS Credentials](#aws-credentials).                                                                                | nil               | `dev`                                              |
| Token Expiration    | `TOKEN_EXPIRATION`    | Token lifetime in seconds. `0` means the default. See [Token Expiration](#token-expiration).                                                                                                           | `900`             | `900`                                              |
| SSL Mode            | `SSLmode`             | psqlODBC's TLS mode: `verify-full`, `verify-ca` or `require`. Other values are replaced with `verify-full`. See [TLS](#tls).                                                                           | `verify-full`     | `verify-ca`                                        |
| libpq Options       | `pqopt`               | psqlODBC's libpq options, in braces. The plugin adds `sslrootcert` (with `verify-full`) and `application_name=aurora-dsql-odbc/<version>` unless you set them. See [TLS](#tls).                        | set by the plugin | `{sslrootcert=/etc/ssl/certs/ca-certificates.crt}` |
| Database Dialect    | `DATABASE_DIALECT`    | Leave unset, or set `AURORA_DSQL`. See [Database Dialect](#database-dialect).                                                                                                                          | detected          | `AURORA_DSQL`                                      |
| Extra URL Encode    | `EXTRA_URL_ENCODE`    | Not needed: Aurora DSQL tokens connect with `0` or `1`.                                                                                                                                                | `0`               | `0`                                                |

`PWD` is ignored: the token replaces it. The plugin also reads psqlODBC's short forms of `SSLmode` and `pqopt`, `CA` and `D5`.

### Installing psqlODBC

On Debian and Ubuntu, install psqlODBC with `sudo apt install odbc-postgresql`. Its Unicode driver is `/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so` (`/usr/lib/aarch64-linux-gnu/odbc/psqlodbcw.so` on Arm), and its ANSI driver, `psqlodbca.so`, is in the same folder. For Windows, install psqlODBC with an installer from [psqlODBC's releases](https://www.postgresql.org/ftp/odbc/releases/).

### Minimal Connection String

> [!NOTE]\
> By default the plugin verifies the server with the system's trusted root certificates: `sslrootcert=system` with libpq 16 or later, and a system CA bundle with libpq 15 or earlier (see below). On Linux, psqlODBC packages use the system's libpq; check its version with `dpkg -s libpq5` (Debian, Ubuntu) or `rpm -q libpq` (RHEL, Fedora, Amazon Linux).

On Linux, with the Unicode wrapper:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

With libpq 15 or earlier the plugin uses the system's CA bundle instead (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem` or `/etc/ssl/cert.pem`, the first that exists and only root can change); set `pqopt={sslrootcert=...}` only where none exists. Most Linux systems include Amazon Root CA 1 in their CA bundle. A root certificate file in `pqopt` works with any libpq version:

```
DRIVER={AWS Advanced ODBC Wrapper Unicode};SERVER=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;pqopt={sslrootcert=/path/to/AmazonRootCA1.pem};
```

Both connect as `admin` to `postgres` with `SSLmode=verify-full`. The region comes from the endpoint, and the credentials from the default AWS credential chain. Use psqlODBC's ANSI driver (`psqlodbca.so`) with the ANSI wrapper; with pyodbc, see [Aurora PostgreSQL](../using-the-aws-odbc-wrapper.md#aurora-postgresql) first. Driver paths vary by system. For Windows, see [Windows](#windows).

### Sample DSN Configuration

#### Linux

In `odbc.ini`, with the wrapper registered in `odbcinst.ini` as in [Getting Started](../../getting-started.md#odbcinstini). On macOS, use the paths of your psqlODBC and wrapper builds and of your root certificate file.

```bash
[ODBC Data Sources]
aurora-dsql     = AWS Advanced ODBC Wrapper Unicode
aurora-dsql-app = AWS Advanced ODBC Wrapper Unicode

# libpq 16 or later, or a system CA bundle (see Minimal Connection String)
[aurora-dsql]
Driver          = AWS Advanced ODBC Wrapper Unicode
SERVER          = abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws
RDS_AUTH_TYPE   = DSQL
BASE_DRIVER     = /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so

# Any libpq version: a custom database role, an AWS profile and the CA bundle of Debian or Ubuntu
[aurora-dsql-app]
Driver          = AWS Advanced ODBC Wrapper Unicode
SERVER          = abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws
RDS_AUTH_TYPE   = DSQL
UID             = app_user
AWS_PROFILE     = dev
pqopt           = {sslrootcert=/etc/ssl/certs/ca-certificates.crt}
BASE_DRIVER     = /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
```

#### Windows

In ODBC Data Source Administrator (64-bit), add a DSN for `AWS Advanced ODBC Wrapper Unicode` or `AWS Advanced ODBC Wrapper Ansi`:

1. **Data Source Name**: a name for the DSN.
2. **Server**: the cluster endpoint.
3. **Username**: leave it blank to connect as `admin`, or enter a custom database role. Leave **Password**, **Port** and **Database** blank.
4. **Base Driver Location**: psqlODBC's driver DLL, `psqlodbc35w.dll` for the Unicode wrapper or `psqlodbc30a.dll` for the ANSI wrapper, in psqlODBC's `bin` folder. Or set **Base Driver Data Source Name** to a psqlODBC DSN, and set that DSN's SSL Mode to `verify-full`: psqlODBC saves `disable` by default, which the plugin replaces, with a warning, on every connect (see [TLS](#tls)).
5. **Base Driver Connection String**: `pqopt={sslrootcert='C:/Program Files/certs/AmazonRootCA1.pem'}`, with forward slashes (see [Root Certificates](#root-certificates)). Separate other psqlODBC options with `;`, for example `SSLmode=verify-ca;pqopt={sslrootcert='C:/Program Files/certs/AmazonRootCA1.pem'}`.
6. **AWS Profile**: optional. See [AWS Credentials](#aws-credentials).
7. **Database Dialect**: leave it blank, or choose **Aurora DSQL**.
8. On the **Authentication** tab, choose **Aurora DSQL** as the **Authentication Mode**. The tab then shows **AWS Region**, **Expire Time** and **IAM Host**. Set **AWS Region** when **Server** isn't the cluster endpoint and no environment variable or AWS profile gives the region (see [Region](#region)). **Expire Time** is in seconds; leave it blank for 900. Leave **IAM Host** blank unless you use [AWS PrivateLink without private DNS](#aws-privatelink-without-private-dns).
9. Choose **Test**, then **Save** and **OK**.

### Region

The token is signed for the cluster's region. The plugin uses the first region it finds:

1. `REGION`
2. The region in `IAM_HOST`, if it's a cluster endpoint
3. The region in `SERVER`, if it's a cluster endpoint
4. The `AWS_REGION` environment variable, then `AWS_DEFAULT_REGION`
5. The `region` in the AWS config file (`~/.aws/config`, or the file `AWS_CONFIG_FILE` names) of the profile named by `AWS_PROFILE`, else by the `AWS_PROFILE` environment variable, else `default`

Spaces, tabs and line breaks around each of these values are dropped, and an empty value, or one with nothing else, such as `REGION= `, counts as unset, so the next source is used. The same goes for the `AWS_PROFILE` key and the `AWS_PROFILE` environment variable (see [AWS Credentials](#aws-credentials)), but not for `SERVER` or `IAM_HOST`, which aren't trimmed. The region is used in lowercase. There's no default region, and the region isn't read from EC2 instance metadata. Aurora DSQL rejects a token signed for another region (see [Troubleshooting](#troubleshooting)).

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

| `SSLmode` you set                                                                                      | Mode used     | Warning logged                                                                   |
|--------------------------------------------------------------------------------------------------------|---------------|----------------------------------------------------------------------------------|
| not set, `verify-full` or `vf`                                                                         | `verify-full` | none                                                                             |
| `verify-ca` or `vc`                                                                                    | `verify-ca`   | `[DSQL] SSLmode=verify-ca verifies the server certificate but not its host name` |
| `require` or `r`                                                                                       | `require`     | `[DSQL] SSLmode=require encrypts but doesn't verify the server certificate`      |
| any other value: `disable`, `allow`, `prefer`, a typo, or psqlODBC's other short forms (`d`, `a`, `p`) | `verify-full` | `[DSQL] Overriding SSLmode=<value> with verify-full`                             |

The mode names match in any case, and the short forms as psqlODBC writes them, in lowercase. See [Logging](../using-the-aws-odbc-wrapper.md#logging) for where the log is.

When the plugin replaces a mode, the connect returns `SQL_SUCCESS_WITH_INFO` with a `01S02` diagnostic record: `SSLmode=<value> was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, verify-ca or require.` If psqlODBC returns its own warning on the same connect, psqlODBC's record is kept and the replacement is only logged. With an output buffer too short for the connection string, the connect still returns this `01S02` record, not `01004`: compare `*StringLength2Ptr` with `BufferLength` to see whether the string was cut.

#### Root Certificates

With `verify-full`, the plugin adds a root certificate to `pqopt` unless you set `sslrootcert`. With libpq 16 or later, or on Windows when it can't read libpq's version, it adds `sslrootcert=system`: libpq then verifies the server against the system's trusted root certificates, which include Amazon Root CA 1 on most systems ([Configuring SSL/TLS certificates](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/configure-root-certificates.html)). libpq 15 or earlier reads `system` as a file name, so with it, or on Linux and macOS when it can't read libpq's version, the plugin adds the system's CA bundle instead: the first of `/etc/ssl/certs/ca-certificates.crt` (Debian, Ubuntu), `/etc/pki/tls/certs/ca-bundle.crt` (RHEL, Fedora, Amazon Linux), `/etc/ssl/ca-bundle.pem` (SUSE) and `/etc/ssl/cert.pem` (macOS) that exists and only root can change. When none does, it adds none, and `verify-full` fails unless you set `sslrootcert`.

The root certificate the plugin adds takes the place of `PGSSLROOTCERT` and `~/.postgresql/root.crt`, which libpq reads only when `sslrootcert` isn't set. To use your own root certificate file, set it with `pqopt={sslrootcert=...}`, as below.

Set a root certificate file instead:

- with libpq 15 or earlier, when none of those CA bundles exists;
- on Windows, where `sslrootcert=system` uses OpenSSL's default certificate paths rather than the Windows certificate store, and can fail with `certificate verify failed` or `SSL error: unregistered scheme`;
- with `verify-ca`: libpq accepts `sslrootcert=system` only with `verify-full`, so the plugin doesn't add it.

On Linux, the system's CA bundle usually includes Amazon Root CA 1: `pqopt={sslrootcert=/etc/ssl/certs/ca-certificates.crt}` on Debian and Ubuntu, `pqopt={sslrootcert=/etc/pki/tls/certs/ca-bundle.crt}` on RHEL, Fedora and Amazon Linux. Otherwise, download [AmazonRootCA1.pem](https://www.amazontrust.com/repository/AmazonRootCA1.pem) and set `pqopt={sslrootcert=/path/to/AmazonRootCA1.pem}`.

> [!WARNING]\
> On Windows, write the path with forward slashes: `pqopt={sslrootcert='C:/Program Files/certs/AmazonRootCA1.pem'}`. libpq reads a backslash in `pqopt` as an escape, so `C:\Program Files\certs\AmazonRootCA1.pem` would become `C:Program FilescertsAmazonRootCA1.pem`. Put a path that contains spaces in single quotes, as here. Keep the file in a folder only administrators can change, such as one under `C:\Program Files`: a folder created directly under `C:\` lets every signed-in user replace it. For a per-user DSN, a folder in your profile works too.

The plugin keeps your other `pqopt` options. It also adds `application_name=aurora-dsql-odbc/<version>` unless you set `application_name`.

### AWS Credentials

The plugin signs tokens with credentials from the AWS SDK for C++:

- Without the `AWS_PROFILE` key, it uses the SDK's default credential chain: environment variables (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`), the profile named by the `AWS_PROFILE` environment variable (else `default`) in `~/.aws/credentials` and `~/.aws/config`, web identity, AWS IAM Identity Center (SSO), then container credentials when `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` or `AWS_CONTAINER_CREDENTIALS_FULL_URI` is set, otherwise the EC2 instance role.
- With the `AWS_PROFILE` key (connection string, DSN, or the dialog's **AWS Profile** field), credentials come only from that profile: access keys, AWS IAM Identity Center (SSO), `aws login`, assume role (`role_arn` with `source_profile` or `credential_source`), web identity (`role_arn` with `web_identity_token_file`) and `credential_process`. Environment variables, container credentials and the instance role aren't used, unless the profile's `credential_source` names them, and a profile that isn't in the AWS config or credentials file fails the connect (see [Troubleshooting](#troubleshooting)).
- With only the `AWS_PROFILE` environment variable, the default chain above runs and reads that profile after the environment variables.

Spaces, tabs and line breaks around the `AWS_PROFILE` key or environment variable are dropped, and an empty value, or one with nothing else, counts as unset: `export AWS_PROFILE=` uses the `default` profile.

The plugin doesn't open a browser for AWS IAM Identity Center (SSO). Sign in with the AWS CLI first, `aws sso login --profile <profile>`, and again when the session expires. For an `aws login` profile, run `aws login --profile <profile>`. Without usable credentials the connect fails before a token is signed; see [Troubleshooting](#troubleshooting).

### Database Dialect

The wrapper picks the Aurora DSQL dialect when `RDS_AUTH_TYPE=DSQL` or the host is a cluster endpoint, so `DATABASE_DIALECT` can stay unset. If you set it, use `AURORA_DSQL` (**Aurora DSQL** in the Windows dialog). With another dialect, such as `AURORA_POSTGRESQL`, the wrapper looks for an Aurora cluster topology that Aurora DSQL doesn't have, and every connect takes several seconds longer.

### Connection Pooling

The plugin signs a new token for each new connection; it doesn't cache tokens. An open connection stays open after its token expires.

Aurora DSQL closes every connection after 60 minutes ([Cluster quotas and database limits](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/CHAP_quotas.html)), and the wrapper doesn't reopen it. Set your connection pool's maximum connection lifetime under 55 minutes, so the pool replaces connections before Aurora DSQL closes them.

### AWS PrivateLink Without Private DNS

Without private DNS, the cluster endpoint doesn't resolve to the VPC endpoint ([Managing clusters using AWS PrivateLink](https://docs.aws.amazon.com/aurora-dsql/latest/userguide/privatelink-managing-clusters.html)). Prefer one of these, which keep `verify-full`:

- Resolve the cluster endpoint to the VPC endpoint yourself, with a Route 53 private hosted zone or a hosts-file entry, and connect to the cluster endpoint as usual.
- Keep `SERVER` the cluster endpoint, and give the VPC endpoint's IP address in libpq's `hostaddr`, for example `pqopt={hostaddr=10.0.1.23}`. libpq then connects to that address and still checks the certificate against `SERVER`. psqlODBC passes one address, so this uses one of the endpoint's network interfaces.

If you connect to the VPC endpoint's DNS name instead, `verify-full` fails because the server certificate doesn't name it. The token must still be signed for the cluster endpoint. Set `IAM_HOST` to the cluster endpoint, `SSLmode` to `verify-ca` with a root certificate file, and the cluster ID in libpq's `options`:

```
SERVER=vpce-0123456789abcdef0-abcdefgh.dsql-fnh4.us-east-1.vpce.amazonaws.com;IAM_HOST=abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws;REGION=us-east-1;RDS_AUTH_TYPE=DSQL;SSLmode=verify-ca;pqopt={sslrootcert=/path/to/AmazonRootCA1.pem options='-c amzn-cluster-id=abcdefghijklmnopqrstuvwxyz'};BASE_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;
```

`verify-ca` checks that the certificate chains to Amazon Root CA 1, not whose it is: any certificate issued under that root passes.

### Troubleshooting

The wrapper's log has more detail; see [Logging](../using-the-aws-odbc-wrapper.md#logging).

#### Missing region for a host name

`DSQL Authentication failed. Missing required parameters: Region (set 'REGION')` (SQLSTATE `08001`)

`SERVER` isn't a cluster endpoint, for example a DNS alias or a VPC endpoint, and no other source has the region. Set `REGION`, or set `IAM_HOST` to the cluster endpoint.

#### No AWS credentials

`Unable to resolve AWS credentials for DSQL authentication with the 'AWS_PROFILE' profile '<profile>'. If it is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile <profile>'.` (SQLSTATE `28000`)

The profile's credentials are missing or expired. For an AWS IAM Identity Center (SSO) profile, run the `aws sso login` command shown, then connect again. For an `aws login` profile the message is the same: run `aws login --profile <profile>` instead. Otherwise check the profile name and its entries in `~/.aws/config` and `~/.aws/credentials`.

`Unable to resolve AWS credentials for DSQL authentication: the 'AWS_PROFILE' profile '<profile>' isn't in the AWS config or credentials file (~/.aws/config, ~/.aws/credentials, or the files AWS_CONFIG_FILE and AWS_SHARED_CREDENTIALS_FILE name). Check the name, or remove AWS_PROFILE to use the default credential chain.` (SQLSTATE `28000`)

The `AWS_PROFILE` key names a profile that neither file has. Check the name, or remove the key to use the default credential chain.

`Unable to resolve AWS credentials for DSQL authentication from the default credential chain (such as environment variables, ~/.aws/credentials, an AWS IAM Identity Center (SSO) session, or a container or instance role)` (SQLSTATE `28000`)

No profile is named, and the default chain found no credentials. Set `AWS_PROFILE`, or provide credentials through one of the chain's sources.

Each fails the connect with `SQL_ERROR` before a token is signed.

#### Token generation failed

`Failed to generate DSQL authentication token. See the driver log for the cause.` (SQLSTATE `28000`)

The credentials resolved, but signing the token failed. The driver log has `Failed to generate an Aurora DSQL token`.

#### SSL mode changed

`SSLmode=<value> was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, verify-ca or require.` (SQLSTATE `01S02`)

A warning: the connect returned `SQL_SUCCESS_WITH_INFO`, and the connection is open with `verify-full`. Set `SSLmode` to one of the three modes, or remove it. The value can come from the connection string, the DSN, or a psqlODBC DSN set as the base DSN.

#### Root certificate errors

With `sslrootcert=system`, the connect fails with a message that contains `root certificate file "system" does not exist`, `certificate verify failed` or `SSL error: unregistered scheme`, and the log has:

`[DSQL] The server certificate couldn't be verified with sslrootcert=system, which needs libpq 16 or later and, on Windows, OpenSSL's default certificate paths. Set pqopt={sslrootcert=<path to AmazonRootCA1.pem>} instead.`

With libpq 15 or earlier the log has one of these instead, for the system CA bundle the plugin added, or when it found none:

`[DSQL] The server certificate couldn't be verified with the system CA bundle, which may lack Amazon Root CA 1. Set pqopt={sslrootcert=<path to AmazonRootCA1.pem>} instead.`

`[DSQL] libpq 15 or earlier has no sslrootcert=system, and no system CA bundle was found. Set pqopt={sslrootcert=<path to AmazonRootCA1.pem>}.`

Set `sslrootcert` to a root certificate file, as in [Root Certificates](#root-certificates).

#### Access denied

`unable to accept connection, access denied` (SQLSTATE `08001`)

Aurora DSQL rejected the token. Check that:

- the credentials allow `dsql:DbConnectAdmin` (for `admin`) or `dsql:DbConnect` (for other roles) on the cluster;
- a custom role is linked to the IAM identity with `AWS IAM GRANT`, and `UID` is the role's name;
- the region is the cluster's region (for a token signed for another region, the message continues with `Credential should be scoped to a valid region.`);
- `IAM_HOST`, if set, is the cluster endpoint.

#### Invalid token expiration

`Invalid value specified for connection string attribute:` followed by `TOKEN_EXPIRATION` (SQLSTATE `01S00`)

`TOKEN_EXPIRATION` must be a whole number of seconds, `0` or more. A value above 604,800 passes this check but fails when the connection opens (see [Token Expiration](#token-expiration)).
