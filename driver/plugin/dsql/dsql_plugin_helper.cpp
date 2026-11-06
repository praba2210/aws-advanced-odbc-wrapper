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

#include "dsql_plugin_helper.h"

#include <cstdint>
#include <string>

#include "../../driver.h"
#include "../../odbcapi_rds_helper.h"
#include "../../util/rds_strings.h"

void DsqlPluginHelper::ReadFirstRecord(STMT* stmt, std::string& sql_state, std::string& message)
{
    // Twice the lengths passed: for an app with a 4-byte SQLWCHAR, RDS_SQLGetDiagRec can copy 2 x MAX_SQL_STATE_LEN and
    // 2 x DIAG_MESSAGE_CHARS SQLTCHARs into them. Smaller buffers would overflow.
    SQLTCHAR state_buffer[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message_buffer[DIAG_MESSAGE_CHARS * 2] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT message_length = 0;
    RDS_SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state_buffer, &native_error, message_buffer, DIAG_MESSAGE_CHARS,
        &message_length, true);
#if UNICODE
    sql_state = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(state_buffer));
    message = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(message_buffer));
#else
    sql_state = reinterpret_cast<const char*>(state_buffer);
    message = reinterpret_cast<const char*>(message_buffer);
#endif
}

std::string DsqlPluginHelper::StatementTextToUtf8(const SQLTCHAR* text)
{
    if (text == nullptr) {
        return "";
    }
#if UNICODE
    // ConvertUTF16ToUTF8 takes a non-const pointer but only reads the text.
    return ConvertUTF16ToUTF8(const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(text)));
#else
    return reinterpret_cast<const char*>(text);
#endif
}

std::string DsqlPluginHelper::AsciiUpper(std::string text)
{
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}
