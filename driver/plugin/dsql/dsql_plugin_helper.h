// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DSQL_PLUGIN_HELPER_H_
#define DSQL_PLUGIN_HELPER_H_

#include "../../util/windows_headers.h"

#include <sql.h>

#include <string>

struct STMT;

// What the Aurora DSQL OCC retry and error hint plugins read from a failed statement, and the ASCII case fold they
// match words with.
namespace DsqlPluginHelper {
    // The message buffer of ReadFirstRecord, in characters. psqlODBC sets a message's record size again on every
    // record-1 read with a buffer (cbErrorMsgMax - 1, environ.c). The hint plugin, the outermost, reads last, so its
    // read sets the size until the application reads record 1 with a buffer of its own; 512 keeps it at psqlODBC's own
    // default, 511 (DRVMNGRDIV). 511 characters are plenty to match on.
    constexpr SQLSMALLINT DIAG_MESSAGE_CHARS = 512;

    // The statement's first diagnostic record: the wrapper's own error if it set one, else the base driver's. Read with
    // RDS_SQLGetDiagRec, which leaves the record for the application (RDS_SQLError would use up SQLError's one record).
    void ReadFirstRecord(STMT* stmt, std::string& sql_state, std::string& message);

    // The statement text the chain got from SQLExecDirect, as UTF-8, to its terminator: DefaultPlugin runs it that way
    // (SQL_NTS), whatever TextLength says. Unicode builds get RDS_SQLExecDirect's NUL-terminated UTF-16 copy. "" for
    // none: SQLExecute passes no text.
    std::string StatementTextToUtf8(const SQLTCHAR* text);

    // The text with a-z upper-cased and every other byte as it was. PostgreSQL folds keywords this way; std::toupper
    // follows the process locale, and ICU's default locale (as in RDS_STR_UPPER) turns i into a dotted capital I under
    // tr and az.
    std::string AsciiUpper(std::string text);
}  // namespace DsqlPluginHelper

#endif // DSQL_PLUGIN_HELPER_H_
