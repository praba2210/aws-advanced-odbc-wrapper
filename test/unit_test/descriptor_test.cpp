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

// Descriptors over a fake base driver: which connection lists a descriptor, who frees it, and which descriptor the
// base driver gets for a statement's ARD and APD. The calls go through the driver's entry points in the order a
// driver manager makes them.

#include "../../driver/util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"

namespace {
    // A record of a base descriptor: what SQLBindCol, SQLBindParameter and SQLSetDescField put there, as psqlODBC
    // keeps it. SQLFetch writes into it, SQLExecute reads from it.
    struct FakeRecord {
        SQLSMALLINT type = 0;
        SQLPOINTER data = nullptr;
        SQLLEN octet_length = 0;
        SQLLEN* indicator = nullptr;
        SQLSMALLINT io_type = SQL_PARAM_INPUT;
    };

    // A base statement, as psqlODBC keeps it: its four implicit descriptors, and its current ARD and APD.
    struct FakeBaseStmt {
        std::map<SQLINTEGER, SQLHDESC> implicit;
        SQLHDESC ard = SQL_NULL_HDESC;
        SQLHDESC apd = SQL_NULL_HDESC;
    };

    // The base driver. Handles are ids it never reuses. Like psqlODBC, SQLSetStmtAttr(SQL_ATTR_APP_ROW_DESC /
    // SQL_ATTR_APP_PARAM_DESC) makes any value the statement's descriptor (NULL its implicit one), and SQLBindCol,
    // SQLBindParameter and SQLExecute use the current one. psqlODBC crashes on a value that isn't one of its live
    // descriptors; here it's counted in `foreign`.
    struct FakeDescBaseDriver {
        std::uintptr_t next_handle = 0x7000;
        std::map<SQLHSTMT, FakeBaseStmt> stmts;
        std::map<SQLHDESC, SQLHDBC> descs;  // live descriptors, the implicit ones too, and their connection
        std::vector<std::pair<SQLHDESC, SQLHDESC>> copies;
        std::vector<std::pair<SQLINTEGER, SQLPOINTER>> desc_sets;  // SQLSetStmtAttr's ARD and APD values
        std::vector<SQLHDESC> bound;  // the descriptor each SQLBindCol and SQLBindParameter wrote to
        int foreign = 0;
        int desc_frees = 0;
        int dead_desc_frees = 0;  // SQLFreeHandle(DESC) on a descriptor that isn't live
        // psqlODBC before 18.00.0002 frees every descriptor of the connection in its SQLDisconnect; 18.00.0002 and later
        // only detach them.
        bool free_descs_on_disconnect = false;
        // The base driver's frees of descriptors ("desc") and its disconnects ("disconnect"), in order.
        std::vector<std::string> order;
        // Each descriptor's records, by record number.
        std::map<SQLHDESC, std::map<SQLUSMALLINT, FakeRecord>> records;
        // Its SQLWCHAR is 4 bytes (a psqlODBC built for iODBC), else 2.
        bool four_byte = false;
        // The value SQLFetch writes into each bound SQL_C_WCHAR column, by column number (UTF-8 here; the base
        // driver's own width in the record's buffer).
        std::map<SQLUSMALLINT, std::string> column_values;
        // For each SQLExecute, the input parameters it read from its APD's records, as UTF-8 (SQL_C_WCHAR in its own
        // width) or the number (SQL_C_SLONG), by parameter number.
        std::vector<std::map<SQLUSMALLINT, std::string>> executed;
        // Each descriptor's header fields that say how many rows or parameter sets a binding has, how they're laid
        // out and at what offset (SQL_DESC_ARRAY_SIZE, SQL_DESC_BIND_TYPE, SQL_DESC_BIND_OFFSET_PTR, and the ODBC 2
        // SQL_ROWSET_SIZE an ARD keeps apart), as psqlODBC keeps them; the statement attributes set and read them.
        std::map<SQLHDESC, std::map<SQLSMALLINT, SQLULEN>> headers;
        std::vector<std::pair<SQLINTEGER, SQLPOINTER>> stmt_attr_sets;  // SQLSetStmtAttr's other attributes
        std::vector<std::pair<SQLHDESC, SQLSMALLINT>> desc_field_sets;  // SQLSetDescField's descriptor and field
        int odbc2_calls = 0;  // SQLSetStmtOption, SQLParamOptions, SQLSetScrollOptions
        std::vector<SQLUSMALLINT> set_pos_operations;
        std::vector<SQLSMALLINT> bulk_operations;
        int desc_rec_sets = 0;
    };
    std::unique_ptr<FakeDescBaseDriver> desc_base_driver;

    SQLHANDLE NextBaseHandle() {
        return reinterpret_cast<SQLHANDLE>(desc_base_driver->next_handle++);
    }

    void UseDesc(SQLHDESC desc) {
        if (!desc_base_driver->descs.contains(desc)) {
            ++desc_base_driver->foreign;
        }
    }

    SQLRETURN FakeAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* output) {
        *output = NextBaseHandle();
        if (type == SQL_HANDLE_DESC) {
            desc_base_driver->descs[*output] = input;
        } else if (type == SQL_HANDLE_STMT) {
            FakeBaseStmt& stmt = desc_base_driver->stmts[*output];
            for (const SQLINTEGER attribute : {SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC, SQL_ATTR_IMP_ROW_DESC,
                                               SQL_ATTR_IMP_PARAM_DESC}) {
                stmt.implicit[attribute] = NextBaseHandle();
                desc_base_driver->descs[stmt.implicit[attribute]] = input;
            }
            stmt.ard = stmt.implicit[SQL_ATTR_APP_ROW_DESC];
            stmt.apd = stmt.implicit[SQL_ATTR_APP_PARAM_DESC];
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeFreeHandle(SQLSMALLINT type, SQLHANDLE handle) {
        if (type == SQL_HANDLE_DESC) {
            ++desc_base_driver->desc_frees;
            desc_base_driver->order.emplace_back("desc");
            if (!desc_base_driver->descs.contains(handle)) {
                ++desc_base_driver->dead_desc_frees;
            }
            desc_base_driver->descs.erase(handle);
            desc_base_driver->records.erase(handle);
        } else if (type == SQL_HANDLE_STMT && desc_base_driver->stmts.contains(handle)) {
            for (const auto& [attribute, desc] : desc_base_driver->stmts[handle].implicit) {
                desc_base_driver->descs.erase(desc);
                desc_base_driver->records.erase(desc);
            }
            desc_base_driver->stmts.erase(handle);
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDisconnect(SQLHDBC dbc) {
        desc_base_driver->order.emplace_back("disconnect");
        if (desc_base_driver->free_descs_on_disconnect) {
            std::erase_if(desc_base_driver->descs, [dbc](const auto& entry) { return entry.second == dbc; });
        }
        return SQL_SUCCESS;
    }

    // The ARD's (true) or APD's header field a statement attribute is.
    std::optional<std::pair<bool, SQLSMALLINT>> HeaderField(SQLINTEGER attribute) {
        switch (attribute) {
            case SQL_ATTR_ROW_ARRAY_SIZE:
                return std::pair{true, static_cast<SQLSMALLINT>(SQL_DESC_ARRAY_SIZE)};
            case SQL_ROWSET_SIZE:
                return std::pair{true, static_cast<SQLSMALLINT>(SQL_ROWSET_SIZE)};
            case SQL_ATTR_ROW_BIND_TYPE:
                return std::pair{true, static_cast<SQLSMALLINT>(SQL_DESC_BIND_TYPE)};
            case SQL_ATTR_ROW_BIND_OFFSET_PTR:
                return std::pair{true, static_cast<SQLSMALLINT>(SQL_DESC_BIND_OFFSET_PTR)};
            case SQL_ATTR_PARAMSET_SIZE:
                return std::pair{false, static_cast<SQLSMALLINT>(SQL_DESC_ARRAY_SIZE)};
            case SQL_ATTR_PARAM_BIND_TYPE:
                return std::pair{false, static_cast<SQLSMALLINT>(SQL_DESC_BIND_TYPE)};
            case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
                return std::pair{false, static_cast<SQLSMALLINT>(SQL_DESC_BIND_OFFSET_PTR)};
            default:
                return std::nullopt;
        }
    }

    // A header field's value, or its default: one row or parameter set, by column, no offset.
    SQLULEN HeaderValue(SQLHDESC desc, SQLSMALLINT field) {
        const std::map<SQLSMALLINT, SQLULEN>& fields = desc_base_driver->headers[desc];
        const auto found = fields.find(field);
        if (found != fields.end()) {
            return found->second;
        }
        return field == SQL_DESC_ARRAY_SIZE || field == SQL_ROWSET_SIZE ? 1 : 0;
    }

    SQLRETURN FakeGetStmtAttr(SQLHSTMT stmt, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER /*length*/,
                              SQLINTEGER* /*out_length*/) {
        FakeBaseStmt& base_stmt = desc_base_driver->stmts.at(stmt);
        if (const auto header = HeaderField(attribute)) {
            *static_cast<SQLULEN*>(value) = HeaderValue(header->first ? base_stmt.ard : base_stmt.apd, header->second);
            return SQL_SUCCESS;
        }
        switch (attribute) {
            case SQL_ATTR_APP_ROW_DESC:
                *static_cast<SQLHDESC*>(value) = base_stmt.ard;
                return SQL_SUCCESS;
            case SQL_ATTR_APP_PARAM_DESC:
                *static_cast<SQLHDESC*>(value) = base_stmt.apd;
                return SQL_SUCCESS;
            case SQL_ATTR_IMP_ROW_DESC:
            case SQL_ATTR_IMP_PARAM_DESC:
                *static_cast<SQLHDESC*>(value) = base_stmt.implicit[attribute];
                return SQL_SUCCESS;
            default:
                return SQL_ERROR;
        }
    }

    SQLRETURN FakeSetStmtAttr(SQLHSTMT stmt, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER /*length*/) {
        FakeBaseStmt& base_stmt = desc_base_driver->stmts.at(stmt);
        if (attribute == SQL_ATTR_APP_ROW_DESC || attribute == SQL_ATTR_APP_PARAM_DESC) {
            desc_base_driver->desc_sets.emplace_back(attribute, value);
            if (value != nullptr) {
                UseDesc(value);
            }
            SQLHDESC& current = attribute == SQL_ATTR_APP_ROW_DESC ? base_stmt.ard : base_stmt.apd;
            current = value != nullptr ? value : base_stmt.implicit[attribute];
            return SQL_SUCCESS;
        }
        desc_base_driver->stmt_attr_sets.emplace_back(attribute, value);
        if (const auto header = HeaderField(attribute)) {
            desc_base_driver->headers[header->first ? base_stmt.ard : base_stmt.apd][header->second] =
                reinterpret_cast<SQLULEN>(value);
        }
        return SQL_SUCCESS;
    }

    // Writes into the current ARD's record; a NULL value unbinds it.
    SQLRETURN FakeBindCol(SQLHSTMT stmt, SQLUSMALLINT column, SQLSMALLINT type, SQLPOINTER value, SQLLEN length,
                          SQLLEN* indicator) {
        const SQLHDESC ard = desc_base_driver->stmts.at(stmt).ard;
        desc_base_driver->bound.push_back(ard);
        UseDesc(ard);
        if (value == nullptr) {
            desc_base_driver->records[ard].erase(column);
        } else {
            desc_base_driver->records[ard][column] = {
                .type = type, .data = value, .octet_length = length, .indicator = indicator};
        }
        return SQL_SUCCESS;
    }

    // Writes into the current APD's record.
    SQLRETURN FakeBindParameter(SQLHSTMT stmt, SQLUSMALLINT number, SQLSMALLINT io_type, SQLSMALLINT value_type,
                                SQLSMALLINT /*param_type*/, SQLULEN /*size*/, SQLSMALLINT /*digits*/, SQLPOINTER value,
                                SQLLEN length, SQLLEN* indicator) {
        const SQLHDESC apd = desc_base_driver->stmts.at(stmt).apd;
        desc_base_driver->bound.push_back(apd);
        UseDesc(apd);
        desc_base_driver->records[apd][number] = {
            .type = value_type, .data = value, .octet_length = length, .indicator = indicator, .io_type = io_type};
        return SQL_SUCCESS;
    }

    // SQL_UNBIND and SQL_RESET_PARAMS empty the current ARD and APD.
    SQLRETURN FakeFreeStmt(SQLHSTMT stmt, SQLUSMALLINT option) {
        const FakeBaseStmt& base_stmt = desc_base_driver->stmts.at(stmt);
        if (option == SQL_UNBIND) {
            desc_base_driver->records[base_stmt.ard].clear();
        } else if (option == SQL_RESET_PARAMS) {
            desc_base_driver->records[base_stmt.apd].clear();
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetDescField(SQLHDESC desc, SQLSMALLINT record, SQLSMALLINT field, SQLPOINTER value,
                               SQLINTEGER /*length*/) {
        UseDesc(desc);
        desc_base_driver->desc_field_sets.emplace_back(desc, field);
        if (field == SQL_DESC_ARRAY_SIZE || field == SQL_DESC_BIND_TYPE || field == SQL_DESC_BIND_OFFSET_PTR) {
            desc_base_driver->headers[desc][field] = reinterpret_cast<SQLULEN>(value);
            return SQL_SUCCESS;
        }
        if (field == SQL_DESC_COUNT) {
            // psqlODBC resets the records above it (column_bindings_set, parameter_bindings_set).
            const auto count = static_cast<SQLSMALLINT>(reinterpret_cast<SQLLEN>(value));
            std::erase_if(desc_base_driver->records[desc],
                          [count](const auto& entry) { return static_cast<int>(entry.first) > count; });
            return SQL_SUCCESS;
        }
        FakeRecord& fields = desc_base_driver->records[desc][static_cast<SQLUSMALLINT>(record)];
        if (field == SQL_DESC_DATA_PTR) {
            fields.data = value;
        } else if (field == SQL_DESC_INDICATOR_PTR || field == SQL_DESC_OCTET_LENGTH_PTR) {
            fields.indicator = static_cast<SQLLEN*>(value);
        } else if (field == SQL_DESC_TYPE || field == SQL_DESC_CONCISE_TYPE) {
            fields.type = static_cast<SQLSMALLINT>(reinterpret_cast<SQLLEN>(value));
        } else if (field == SQL_DESC_OCTET_LENGTH) {
            fields.octet_length = reinterpret_cast<SQLLEN>(value);
        }
        return SQL_SUCCESS;
    }

    // A record's type, 0 for one it doesn't have.
    SQLRETURN FakeGetDescField(SQLHDESC desc, SQLSMALLINT record, SQLSMALLINT field, SQLPOINTER value,
                               SQLINTEGER /*length*/, SQLINTEGER* /*out_length*/) {
        UseDesc(desc);
        if (field != SQL_DESC_TYPE && field != SQL_DESC_CONCISE_TYPE) {
            return SQL_ERROR;
        }
        const std::map<SQLUSMALLINT, FakeRecord>& fields = desc_base_driver->records[desc];
        const auto found = fields.find(static_cast<SQLUSMALLINT>(record));
        *static_cast<SQLSMALLINT*>(value) = found != fields.end() ? found->second.type : 0;
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetDescRec(SQLHDESC desc, SQLSMALLINT record, SQLSMALLINT type, SQLSMALLINT /*subtype*/,
                             SQLLEN length, SQLSMALLINT /*precision*/, SQLSMALLINT /*scale*/, SQLPOINTER data,
                             SQLLEN* /*string_length*/, SQLLEN* indicator) {
        UseDesc(desc);
        ++desc_base_driver->desc_rec_sets;
        desc_base_driver->records[desc][static_cast<SQLUSMALLINT>(record)] = {
            .type = type, .data = data, .octet_length = length, .indicator = indicator};
        return SQL_SUCCESS;
    }

#if UNICODE && !defined(_WIN32)
    // A value in the base driver's width into a record's buffer, as psqlODBC writes it: as much as the record's
    // octet length holds with a NUL, and the whole length.
    template <typename Unit>
    void WriteBaseUnits(const Unit* text, size_t units, const FakeRecord& record) {
        const size_t room = record.octet_length > 0 ? static_cast<size_t>(record.octet_length) / sizeof(Unit) : 0;
        const size_t written = room > 0 ? std::min(units, room - 1) : 0;
        if (room > 0) {
            const Unit nul = 0;
            std::memcpy(record.data, text, written * sizeof(Unit));
            std::memcpy(static_cast<char*>(record.data) + (written * sizeof(Unit)), &nul, sizeof(Unit));
        }
        if (record.indicator != nullptr) {
            *record.indicator = static_cast<SQLLEN>(units * sizeof(Unit));
        }
    }

    void WriteBaseText(const std::string& value, const FakeRecord& record) {
        if (desc_base_driver->four_byte) {
            const std::wstring text = ConvertUTF8ToWString(value);
            WriteBaseUnits(text.data(), text.size(), record);
        } else {
            const std::vector<uint16_t> text = ConvertUTF8ToUTF16(value);
            WriteBaseUnits(text.data(), text.size() - 1, record);
        }
    }

    // A record's SQL_C_WCHAR value in the base driver's width, to its indicator's length or its NUL.
    std::string ReadBaseText(const FakeRecord& record) {
        const SQLLEN length = record.indicator != nullptr ? *record.indicator : SQL_NTS;
        if (length == SQL_NULL_DATA) {
            return "NULL";
        }
        const SQLLEN unit = desc_base_driver->four_byte ? 4 : 2;
        const auto chars = static_cast<SQLINTEGER>(length == SQL_NTS ? SQL_NTS : length / unit);
        return ConvertUserAppToUTF8(desc_base_driver->four_byte, static_cast<SQLTCHAR*>(record.data), chars);
    }
#endif

    // Writes the current ARD's bound SQL_C_WCHAR columns.
    SQLRETURN FakeFetch(SQLHSTMT stmt) {
        const SQLHDESC ard = desc_base_driver->stmts.at(stmt).ard;
        UseDesc(ard);
#if UNICODE && !defined(_WIN32)
        for (const auto& [column, record] : desc_base_driver->records[ard]) {
            const auto value = desc_base_driver->column_values.find(column);
            if (record.type == SQL_C_WCHAR && record.data != nullptr && value != desc_base_driver->column_values.end()) {
                WriteBaseText(value->second, record);
            }
        }
#endif
        return SQL_SUCCESS;
    }

    // Reads the current APD's input parameters.
    SQLRETURN FakeExecute(SQLHSTMT stmt) {
        UseDesc(desc_base_driver->stmts.at(stmt).ard);
        const SQLHDESC apd = desc_base_driver->stmts.at(stmt).apd;
        UseDesc(apd);
        std::map<SQLUSMALLINT, std::string> read;
        for (const auto& [number, record] : desc_base_driver->records[apd]) {
            if (record.data == nullptr || record.io_type == SQL_PARAM_OUTPUT) {
                continue;
            }
            if (record.type == SQL_C_SLONG) {
                read[number] = std::to_string(*static_cast<SQLINTEGER*>(record.data));
            }
#if UNICODE && !defined(_WIN32)
            if (record.type == SQL_C_WCHAR) {
                read[number] = ReadBaseText(record);
            }
#endif
        }
        desc_base_driver->executed.push_back(read);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetCursorName(SQLHSTMT /*stmt*/, SQLTCHAR* /*name*/, SQLSMALLINT /*length*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetPos(SQLHSTMT /*stmt*/, SQLSETPOSIROW /*row*/, SQLUSMALLINT operation, SQLUSMALLINT /*lock*/) {
        desc_base_driver->set_pos_operations.push_back(operation);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeBulkOperations(SQLHSTMT /*stmt*/, SQLSMALLINT operation) {
        desc_base_driver->bulk_operations.push_back(operation);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetStmtOption(SQLHSTMT /*stmt*/, SQLUSMALLINT /*option*/, SQLULEN /*value*/) {
        ++desc_base_driver->odbc2_calls;
        return SQL_SUCCESS;
    }

    SQLRETURN FakeParamOptions(SQLHSTMT /*stmt*/, SQLULEN /*sets*/, SQLULEN* /*processed*/) {
        ++desc_base_driver->odbc2_calls;
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetScrollOptions(SQLHSTMT /*stmt*/, SQLUSMALLINT /*concurrency*/, SQLLEN /*keyset*/,
                                   SQLUSMALLINT /*rowset*/) {
        ++desc_base_driver->odbc2_calls;
        return SQL_SUCCESS;
    }

    // Gives the target the source's records, as psqlODBC's PGAPI_CopyDesc does: the same buffer pointers.
    SQLRETURN FakeCopyDesc(SQLHDESC source, SQLHDESC target) {
        desc_base_driver->copies.emplace_back(source, target);
        desc_base_driver->records[target] = desc_base_driver->records[source];
        return SQL_SUCCESS;
    }

    template <typename Function>
    RdsPlatform::FuncHandle AsFuncHandle(Function* function) {
        return reinterpret_cast<RdsPlatform::FuncHandle>(function);
    }

    class FakeDescBaseDriverLoader : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& name) override {
            static const std::map<std::string_view, RdsPlatform::FuncHandle> FUNCTIONS = {
                {RdsFuncNames::AllocHandle, AsFuncHandle(&FakeAllocHandle)},
                {RdsFuncNames::FreeHandle, AsFuncHandle(&FakeFreeHandle)},
                {RdsFuncNames::Disconnect, AsFuncHandle(&FakeDisconnect)},
                {RdsFuncNames::GetStmtAttr, AsFuncHandle(&FakeGetStmtAttr)},
                {RdsFuncNames::SetStmtAttr, AsFuncHandle(&FakeSetStmtAttr)},
                {RdsFuncNames::BindCol, AsFuncHandle(&FakeBindCol)},
                {RdsFuncNames::BindParameter, AsFuncHandle(&FakeBindParameter)},
                {RdsFuncNames::Execute, AsFuncHandle(&FakeExecute)},
                {RdsFuncNames::SetCursorName, AsFuncHandle(&FakeSetCursorName)},
                {RdsFuncNames::CopyDesc, AsFuncHandle(&FakeCopyDesc)},
                {RdsFuncNames::Fetch, AsFuncHandle(&FakeFetch)},
                {RdsFuncNames::FreeStmt, AsFuncHandle(&FakeFreeStmt)},
                {RdsFuncNames::SetDescField, AsFuncHandle(&FakeSetDescField)},
                {RdsFuncNames::SetStmtOption, AsFuncHandle(&FakeSetStmtOption)},
                {RdsFuncNames::SetPos, AsFuncHandle(&FakeSetPos)},
                {RdsFuncNames::GetDescField, AsFuncHandle(&FakeGetDescField)},
                {RdsFuncNames::SetDescRec, AsFuncHandle(&FakeSetDescRec)},
                {RdsFuncNames::BulkOperations, AsFuncHandle(&FakeBulkOperations)},
                {RdsFuncNames::ParamOptions, AsFuncHandle(&FakeParamOptions)},
                {RdsFuncNames::SetScrollOptions, AsFuncHandle(&FakeSetScrollOptions)},
            };
            const auto found = FUNCTIONS.find(name);
            return found != FUNCTIONS.end() ? found->second : nullptr;
        }
    };

    // The plugin service of a connected handle, for its OdbcHelper.
    class HelperOnlyPluginService : public PluginService {
    public:
        explicit HelperOnlyPluginService(std::shared_ptr<OdbcHelper> odbc_helper)
            : odbc_helper_(std::move(odbc_helper)) {}

        std::shared_ptr<OdbcHelper> GetOdbcHelper() override { return odbc_helper_; }

    private:
        std::shared_ptr<OdbcHelper> odbc_helper_;
    };

    DESC* AsDesc(SQLHDESC handle) {
        return static_cast<DESC*>(handle);
    }

    STMT* AsStmt(SQLHSTMT handle) {
        return static_cast<STMT*>(handle);
    }

#if UNICODE && !defined(_WIN32)
    // An app's SQL_C_WCHAR buffer of BYTES bytes and its StrLen_or_Ind, filled with a sentinel. A 4-byte NUL after
    // it, which the app doesn't bind, ends its text when nothing was written.
    struct AppBuffer {
        static constexpr SQLLEN BYTES = 64;
        static constexpr SQLCHAR SENTINEL = 0xAB;
        static constexpr SQLLEN SENTINEL_LENGTH = -999;
        std::vector<SQLCHAR> bytes = Filled();
        SQLLEN indicator = SENTINEL_LENGTH;

        static std::vector<SQLCHAR> Filled() {
            std::vector<SQLCHAR> filled(BYTES + 4, 0);
            std::fill(filled.begin(), filled.begin() + BYTES, SENTINEL);
            return filled;
        }

        SQLPOINTER Get() { return bytes.data(); }

        // Its text in the app's width, to its NUL.
        std::string Text(bool four_byte_app) {
            return ConvertUserAppToUTF8(four_byte_app, reinterpret_cast<SQLTCHAR*>(bytes.data()), SQL_NTS);
        }

        // Puts an input value in it, in the app's width, with SQL_NTS.
        void Set(const std::string& text, bool four_byte_app) {
            std::fill(bytes.begin(), bytes.end(), 0);
            if (four_byte_app) {
                const std::wstring wide = ConvertUTF8ToWString(text);
                std::memcpy(bytes.data(), wide.data(), wide.size() * sizeof(wchar_t));
            } else {
                const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
                std::memcpy(bytes.data(), utf16.data(), (utf16.size() - 1) * sizeof(uint16_t));
            }
            indicator = SQL_NTS;
        }

        // The sentinel again, in the same storage (a copy may still point at it).
        void Reset() {
            std::fill(bytes.begin(), bytes.begin() + BYTES, SENTINEL);
            indicator = SENTINEL_LENGTH;
        }

        [[nodiscard]] bool Untouched() const {
            return indicator == SENTINEL_LENGTH
                && std::all_of(bytes.begin(), bytes.begin() + BYTES, [](SQLCHAR byte) { return byte == SENTINEL; });
        }
    };
#endif
}  // namespace

class DescriptorTest : public testing::Test {
protected:
    ENV env_;
    // The base connection handles; only their addresses are used.
    int base_dbc_ = 0;
    int other_base_dbc_ = 0;
    std::shared_ptr<OdbcHelper> odbc_helper_;
    SQLINTEGER value_ = 0;
    SQLLEN indicator_ = 0;

    void SetUp() override {
        desc_base_driver = std::make_unique<FakeDescBaseDriver>();
        env_.driver_lib_loader = std::make_shared<FakeDescBaseDriverLoader>();
        odbc_helper_ = std::make_shared<OdbcHelper>(env_.driver_lib_loader, &env_);
    }

    void TearDown() override {
        env_.dbc_list.clear();
        desc_base_driver = nullptr;
    }

    // SQLAllocHandle(DBC), and what SQLConnect leaves: the base connection and a plugin service.
    DBC* Connect(int* base_dbc) {
        SQLHDBC handle = SQL_NULL_HDBC;
        EXPECT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, &env_, &handle));
        DBC* dbc = static_cast<DBC*>(handle);
        dbc->wrapped_dbc = base_dbc;
        dbc->conn_status = CONN_CONNECTED;
        dbc->plugin_service = std::make_shared<HelperOnlyPluginService>(odbc_helper_);
        return dbc;
    }

    // SQLAllocHandle(STMT) as unixODBC and iODBC run it: right after it, they get the four implicit descriptors.
    static SQLHSTMT AllocStmt(DBC* dbc) {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        EXPECT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
        for (const SQLINTEGER attribute : {SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC, SQL_ATTR_IMP_ROW_DESC,
                                           SQL_ATTR_IMP_PARAM_DESC}) {
            GetDesc(stmt, attribute);
        }
        return stmt;
    }

    static SQLHDESC GetDesc(SQLHSTMT stmt, SQLINTEGER attribute) {
        SQLHDESC desc = SQL_NULL_HDESC;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetStmtAttr(stmt, attribute, &desc, SQL_IS_POINTER, nullptr));
        return desc;
    }

    static SQLHDESC AllocDesc(DBC* dbc) {
        SQLHDESC desc = SQL_NULL_HDESC;
        EXPECT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DESC, dbc, &desc));
        return desc;
    }

    // The base statement's own implicit descriptor for attribute.
    static SQLHDESC BaseImplicit(SQLHSTMT stmt, SQLINTEGER attribute) {
        return desc_base_driver->stmts.at(AsStmt(stmt)->wrapped_stmt).implicit.at(attribute);
    }

    SQLRETURN BindColumn(SQLHSTMT stmt) {
        return SQLBindCol(stmt, 1, SQL_C_SLONG, &value_, 0, &indicator_);
    }

    SQLRETURN BindParameter(SQLHSTMT stmt) {
        return SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &value_, 0, &indicator_);
    }

    static void Disconnect(DBC* dbc) {
        dbc->plugin_head = nullptr;
        EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
    }

#if UNICODE && !defined(_WIN32)
    // The app's and the base driver's SQLWCHAR differ, so the wrapper keeps a copy of each SQL_C_WCHAR binding: a
    // 4-byte app over a 2-byte base driver (iODBC on macOS), or a 2-byte app over a 4-byte one.
    void UseWidths(bool four_byte_app) {
        env_.use_4_bytes_user_app = four_byte_app;
        odbc_helper_->SetUse4BytesBaseDriver(!four_byte_app);
        desc_base_driver->four_byte = !four_byte_app;
    }

    // Both SQLWCHARs the same width, 2 bytes or 4 (an iODBC app over a psqlODBC built for iODBC), over a fresh base
    // driver: the wrapper keeps no copies.
    void UseMatchedWidths(bool four_byte) {
        desc_base_driver = std::make_unique<FakeDescBaseDriver>();
        env_.use_4_bytes_user_app = four_byte;
        odbc_helper_->SetUse4BytesBaseDriver(four_byte);
        desc_base_driver->four_byte = four_byte;
    }

    static SQLRETURN BindTextColumn(SQLHSTMT stmt, SQLUSMALLINT column, AppBuffer& buffer) {
        return SQLBindCol(stmt, column, SQL_C_WCHAR, buffer.Get(), AppBuffer::BYTES, &buffer.indicator);
    }

    static SQLRETURN BindTextParameter(SQLHSTMT stmt, AppBuffer& buffer) {
        return SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, buffer.Get(),
                                AppBuffer::BYTES, &buffer.indicator);
    }

    static std::string StateOf(const std::unique_ptr<ErrInfo>& err) {
        return err != nullptr ? err->sqlstate : std::string("no record");
    }

    // "abc" in the app's bytes.
    static SQLLEN AbcLength(bool four_byte_app) {
        return four_byte_app ? 12 : 6;
    }
#endif
};

// A statement's implicit descriptor stays its statement's: a copy into it doesn't list it on the connection, so the
// connection's teardown doesn't touch it after the statement freed it. It did, and SQLFreeHandle(DBC) read it.
TEST_F(DescriptorTest, CopyIntoAnImplicitDescriptorDoesntListIt) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT source_stmt = AllocStmt(dbc);
    SQLHSTMT target_stmt = AllocStmt(dbc);
    SQLHDESC source = GetDesc(source_stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC target = GetDesc(target_stmt, SQL_ATTR_APP_ROW_DESC);

    EXPECT_EQ(SQL_SUCCESS, SQLCopyDesc(source, target));

    ASSERT_EQ(1U, desc_base_driver->copies.size());
    EXPECT_EQ(AsDesc(source)->wrapped_desc, desc_base_driver->copies[0].first);
    EXPECT_EQ(AsDesc(target)->wrapped_desc, desc_base_driver->copies[0].second);
    EXPECT_TRUE(dbc->desc_list.empty());
    EXPECT_EQ(dbc, AsDesc(target)->dbc);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, target_stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, source_stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
}

// A copy from another connection's descriptor changes neither connection's list nor the targets' connection. It
// moved the target to the source's connection, whose teardown then freed a descriptor its owner still had.
TEST_F(DescriptorTest, CopyAcrossConnectionsLeavesBothConnectionsUnchanged) {
    DBC* source_dbc = Connect(&base_dbc_);
    DBC* target_dbc = Connect(&other_base_dbc_);
    SQLHSTMT source_stmt = AllocStmt(source_dbc);
    SQLHSTMT target_stmt = AllocStmt(target_dbc);
    SQLHDESC source = GetDesc(source_stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC source_explicit = AllocDesc(source_dbc);
    SQLHDESC implicit_target = GetDesc(target_stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC explicit_target = AllocDesc(target_dbc);

    EXPECT_EQ(SQL_SUCCESS, SQLCopyDesc(source, implicit_target));
    EXPECT_EQ(SQL_SUCCESS, SQLCopyDesc(source, explicit_target));

    EXPECT_EQ(2U, desc_base_driver->copies.size());
    EXPECT_EQ(std::list<DESC*>{AsDesc(source_explicit)}, source_dbc->desc_list);
    EXPECT_EQ(std::list<DESC*>{AsDesc(explicit_target)}, target_dbc->desc_list);
    EXPECT_EQ(target_dbc, AsDesc(implicit_target)->dbc);
    EXPECT_EQ(target_dbc, AsDesc(explicit_target)->dbc);
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(source_dbc));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, source_dbc));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, target_stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, explicit_target));
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(target_dbc));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, target_dbc));
}

// SQLFreeHandle on a statement's implicit descriptor returns HY017, as ODBC says, and leaves it to its statement. It
// deleted the descriptor, and the statement's own free then deleted it again.
TEST_F(DescriptorTest, FreeHandleOnAnImplicitDescriptorReturnsHY017) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    for (const SQLINTEGER attribute : {SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC, SQL_ATTR_IMP_ROW_DESC,
                                       SQL_ATTR_IMP_PARAM_DESC}) {
        SQLHDESC desc = GetDesc(stmt, attribute);
        EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DESC, desc));
        ASSERT_NE(nullptr, AsDesc(desc)->err);
        EXPECT_EQ("HY017", AsDesc(desc)->err->sqlstate);
        EXPECT_NE(nullptr, AsDesc(desc)->wrapped_desc);
    }
    EXPECT_EQ(0, desc_base_driver->desc_frees);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
}

// An explicit descriptor the app makes a statement's ARD reaches the base driver as the base driver's own descriptor,
// so SQLBindCol binds in it. The base driver got the wrapper's DESC, and psqlODBC crashed in SQLBindCol.
TEST_F(DescriptorTest, BindColumnOnAnExplicitArdGoesToItsBaseDescriptor) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC ard = AllocDesc(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, BindColumn(stmt));

    EXPECT_EQ(std::vector<SQLHDESC>{AsDesc(ard)->wrapped_desc}, desc_base_driver->bound);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    Disconnect(dbc);
}

// The same for an APD and SQLBindParameter, where psqlODBC crashed too.
TEST_F(DescriptorTest, BindParameterOnAnExplicitApdGoesToItsBaseDescriptor) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC apd = AllocDesc(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, apd, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, BindParameter(stmt));

    EXPECT_EQ(std::vector<SQLHDESC>{AsDesc(apd)->wrapped_desc}, desc_base_driver->bound);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, apd));
    Disconnect(dbc);
}

// SQLSetStmtAttr with NULL after an explicit ARD puts the base statement back on its implicit ARD.
TEST_F(DescriptorTest, NullPutsTheImplicitArdBack) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC ard = AllocDesc(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, BindColumn(stmt));

    ASSERT_EQ(2U, desc_base_driver->desc_sets.size());
    EXPECT_EQ(nullptr, desc_base_driver->desc_sets[1].second);
    EXPECT_EQ(std::vector<SQLHDESC>{BaseImplicit(stmt, SQL_ATTR_APP_ROW_DESC)}, desc_base_driver->bound);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    Disconnect(dbc);
}

// The statement's own implicit ARD does the same: an app sets it back that way, and unixODBC passes it for an app's
// NULL to a driver that has only SQLSetStmtAttrW. The base driver got the wrapper's implicit DESC, and psqlODBC crashed.
TEST_F(DescriptorTest, ItsOwnImplicitArdPutsTheImplicitArdBack) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC implicit_ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC ard = AllocDesc(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, implicit_ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, BindColumn(stmt));

    ASSERT_EQ(2U, desc_base_driver->desc_sets.size());
    EXPECT_EQ(nullptr, desc_base_driver->desc_sets[1].second);
    EXPECT_EQ(std::vector<SQLHDESC>{BaseImplicit(stmt, SQL_ATTR_APP_ROW_DESC)}, desc_base_driver->bound);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    Disconnect(dbc);
}

// An explicit descriptor without a base descriptor (failover and a read/write splitting switch drop them) gets one on
// the connection's base connection first.
TEST_F(DescriptorTest, ExplicitDescriptorWithoutABaseDescriptorGetsOne) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC ard = AllocDesc(dbc);
    const SQLHDESC dropped = AsDesc(ard)->wrapped_desc;
    AsDesc(ard)->wrapped_desc = SQL_NULL_HDESC;

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));

    ASSERT_NE(nullptr, AsDesc(ard)->wrapped_desc);
    EXPECT_NE(dropped, AsDesc(ard)->wrapped_desc);
    ASSERT_TRUE(desc_base_driver->descs.contains(AsDesc(ard)->wrapped_desc));
    EXPECT_EQ(&base_dbc_, desc_base_driver->descs.at(AsDesc(ard)->wrapped_desc));
    ASSERT_EQ(1U, desc_base_driver->desc_sets.size());
    EXPECT_EQ(AsDesc(ard)->wrapped_desc, desc_base_driver->desc_sets[0].second);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    Disconnect(dbc);
}

// Another connection's explicit descriptor gets HY024, and nothing reaches the base driver.
TEST_F(DescriptorTest, AnotherConnectionsDescriptorGetsHY024) {
    DBC* dbc = Connect(&base_dbc_);
    DBC* other_dbc = Connect(&other_base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC other_desc = AllocDesc(other_dbc);

    for (const SQLINTEGER attribute : {SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC}) {
        EXPECT_EQ(SQL_ERROR, RDS_SQLSetStmtAttr(stmt, attribute, other_desc, SQL_IS_POINTER));
        ASSERT_NE(nullptr, AsStmt(stmt)->err);
        EXPECT_EQ("HY024", AsStmt(stmt)->err->sqlstate);
    }

    EXPECT_TRUE(desc_base_driver->desc_sets.empty());
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, other_desc));
    Disconnect(other_dbc);
    Disconnect(dbc);
}

// The descriptors the app left for the disconnect lose their base descriptors before the base disconnect, once. A
// psqlODBC before 18.00.0002 frees them in its disconnect, and the connection's later free freed them again.
TEST_F(DescriptorTest, Disconnect_FreesTheAppsDescriptorsBeforeTheBaseDisconnect) {
    for (const bool free_descs_on_disconnect : {true, false}) {
        SCOPED_TRACE(free_descs_on_disconnect ? "psqlODBC 16" : "psqlODBC 18");
        desc_base_driver = std::make_unique<FakeDescBaseDriver>();
        desc_base_driver->free_descs_on_disconnect = free_descs_on_disconnect;
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindColumn(stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));

        Disconnect(dbc);

        EXPECT_EQ((std::vector<std::string>{"desc", "disconnect"}), desc_base_driver->order);
        EXPECT_EQ(1, desc_base_driver->desc_frees);
        EXPECT_EQ(0, desc_base_driver->dead_desc_frees);
    }
}

TEST_F(DescriptorTest, Disconnect_ADescriptorNeverAttachedIsFreedFirstToo) {
    for (const bool free_descs_on_disconnect : {true, false}) {
        SCOPED_TRACE(free_descs_on_disconnect ? "psqlODBC 16" : "psqlODBC 18");
        desc_base_driver = std::make_unique<FakeDescBaseDriver>();
        desc_base_driver->free_descs_on_disconnect = free_descs_on_disconnect;
        DBC* dbc = Connect(&base_dbc_);
        AllocDesc(dbc);

        Disconnect(dbc);

        EXPECT_EQ((std::vector<std::string>{"desc", "disconnect"}), desc_base_driver->order);
        EXPECT_EQ(0, desc_base_driver->dead_desc_frees);
    }
}

// No driver manager frees the descriptor after the disconnect: the DESC is still the app's, and its free makes no
// base call.
TEST_F(DescriptorTest, Disconnect_ThenTheAppFreesTheDescriptor_NoBaseCall) {
    for (const bool free_descs_on_disconnect : {true, false}) {
        SCOPED_TRACE(free_descs_on_disconnect ? "psqlODBC 16" : "psqlODBC 18");
        desc_base_driver = std::make_unique<FakeDescBaseDriver>();
        desc_base_driver->free_descs_on_disconnect = free_descs_on_disconnect;
        DBC* dbc = Connect(&base_dbc_);
        SQLHDESC desc = AllocDesc(dbc);
        dbc->plugin_head = nullptr;
        EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
        const int frees = desc_base_driver->desc_frees;

        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, desc));

        EXPECT_EQ(frees, desc_base_driver->desc_frees);
        EXPECT_EQ(0, desc_base_driver->dead_desc_frees);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
    }
}

// After a reconnect on the same handle the descriptor gets a base descriptor again when a statement uses it.
TEST_F(DescriptorTest, Reconnect_SetStmtAttrGivesTheDescriptorANewBaseDescriptor) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHDESC ard = AllocDesc(dbc);
    dbc->plugin_head = nullptr;
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    ASSERT_EQ(nullptr, AsDesc(ard)->wrapped_desc);
    dbc->conn_status = CONN_CONNECTED;
    SQLHSTMT stmt = AllocStmt(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));

    ASSERT_NE(nullptr, AsDesc(ard)->wrapped_desc);
    EXPECT_TRUE(desc_base_driver->descs.contains(AsDesc(ard)->wrapped_desc));
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    Disconnect(dbc);
}

// An implicit descriptor other than the statement's own ARD or APD (its IRD or IPD, its APD as the ARD, another
// statement's ARD) gets HY017, and nothing reaches the base driver.
TEST_F(DescriptorTest, AnotherImplicitDescriptorGetsHY017) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHSTMT other_stmt = AllocStmt(dbc);
    const std::vector<std::pair<SQLINTEGER, SQLHDESC>> refused = {
        {SQL_ATTR_APP_ROW_DESC, GetDesc(stmt, SQL_ATTR_IMP_ROW_DESC)},
        {SQL_ATTR_APP_PARAM_DESC, GetDesc(stmt, SQL_ATTR_IMP_PARAM_DESC)},
        {SQL_ATTR_APP_ROW_DESC, GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC)},
        {SQL_ATTR_APP_ROW_DESC, GetDesc(other_stmt, SQL_ATTR_APP_ROW_DESC)},
    };

    for (const auto& [attribute, desc] : refused) {
        EXPECT_EQ(SQL_ERROR, RDS_SQLSetStmtAttr(stmt, attribute, desc, SQL_IS_POINTER));
        ASSERT_NE(nullptr, AsStmt(stmt)->err);
        EXPECT_EQ("HY017", AsStmt(stmt)->err->sqlstate);
    }

    EXPECT_TRUE(desc_base_driver->desc_sets.empty());
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other_stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    Disconnect(dbc);
}

// SQLGetStmtAttr returns the descriptor the app set (both driver managers answer it themselves; without one the app got
// the wrong one), and the implicit descriptor keeps its own base handle. It returned the implicit descriptor and put
// the base driver's answer, the explicit one's base descriptor, in it.
TEST_F(DescriptorTest, GetStmtAttrReturnsTheDescriptorTheAppSet) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC implicit_ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC implicit_apd = GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC);
    SQLHDESC ard = AllocDesc(dbc);
    SQLHDESC apd = AllocDesc(dbc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, apd, SQL_IS_POINTER));
    EXPECT_EQ(ard, GetDesc(stmt, SQL_ATTR_APP_ROW_DESC));
    EXPECT_EQ(apd, GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC));
    EXPECT_EQ(BaseImplicit(stmt, SQL_ATTR_APP_ROW_DESC), AsDesc(implicit_ard)->wrapped_desc);
    EXPECT_EQ(BaseImplicit(stmt, SQL_ATTR_APP_PARAM_DESC), AsDesc(implicit_apd)->wrapped_desc);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));
    EXPECT_EQ(implicit_ard, GetDesc(stmt, SQL_ATTR_APP_ROW_DESC));
    EXPECT_EQ(BaseImplicit(stmt, SQL_ATTR_APP_ROW_DESC), AsDesc(implicit_ard)->wrapped_desc);
    EXPECT_EQ(apd, GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, apd));
    Disconnect(dbc);
}

// After failover or a read/write splitting switch, the statement's new base statement gets the explicit ARD and APD as
// base descriptors on the new base connection. It got nothing (and before that, the wrapper's DESC from attr_map).
TEST_F(DescriptorTest, NewBaseStatementGetsTheExplicitDescriptorsOnTheNewConnection) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT stmt = AllocStmt(dbc);
    SQLHDESC ard = AllocDesc(dbc);
    SQLHDESC apd = AllocDesc(dbc);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, apd, SQL_IS_POINTER));
    // What failover's reconnect does: the connection's base handles go, and it gets a new base connection.
    {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        OdbcHelper::ReleaseBaseHandles(env_.driver_lib_loader, dbc, true);
    }
    dbc->wrapped_dbc = &other_base_dbc_;

    DefaultPlugin plugin(dbc);
    EXPECT_EQ(SQL_SUCCESS, plugin.Execute(stmt, nullptr, 0));

    ASSERT_NE(nullptr, AsStmt(stmt)->wrapped_stmt);
    const FakeBaseStmt& base_stmt = desc_base_driver->stmts.at(AsStmt(stmt)->wrapped_stmt);
    EXPECT_EQ(AsDesc(ard)->wrapped_desc, base_stmt.ard);
    EXPECT_EQ(AsDesc(apd)->wrapped_desc, base_stmt.apd);
    ASSERT_TRUE(desc_base_driver->descs.contains(base_stmt.ard));
    ASSERT_TRUE(desc_base_driver->descs.contains(base_stmt.apd));
    EXPECT_EQ(&other_base_dbc_, desc_base_driver->descs.at(base_stmt.ard));
    EXPECT_EQ(&other_base_dbc_, desc_base_driver->descs.at(base_stmt.apd));
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, apd));
    Disconnect(dbc);
}

// Freeing an explicit descriptor puts the statements that use it back on their implicit descriptors, as ODBC says, in
// the base driver too. Both kept the freed descriptor, and the base statements its freed base descriptor.
TEST_F(DescriptorTest, FreeingAnExplicitDescriptorPutsItsStatementsBackOnTheirImplicitOnes) {
    DBC* dbc = Connect(&base_dbc_);
    SQLHSTMT fetch_stmt = AllocStmt(dbc);
    SQLHSTMT insert_stmt = AllocStmt(dbc);
    SQLHDESC implicit_ard = GetDesc(fetch_stmt, SQL_ATTR_APP_ROW_DESC);
    SQLHDESC implicit_apd = GetDesc(insert_stmt, SQL_ATTR_APP_PARAM_DESC);
    SQLHDESC shared = AllocDesc(dbc);
    // One descriptor as one statement's ARD and the other's APD (the ODBC reference's way to copy rows).
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(fetch_stmt, SQL_ATTR_APP_ROW_DESC, shared, SQL_IS_POINTER));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(insert_stmt, SQL_ATTR_APP_PARAM_DESC, shared, SQL_IS_POINTER));

    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, shared));

    EXPECT_EQ(implicit_ard, GetDesc(fetch_stmt, SQL_ATTR_APP_ROW_DESC));
    EXPECT_EQ(implicit_apd, GetDesc(insert_stmt, SQL_ATTR_APP_PARAM_DESC));
    EXPECT_EQ(SQL_SUCCESS, BindColumn(fetch_stmt));
    EXPECT_EQ(SQL_SUCCESS, BindParameter(insert_stmt));
    EXPECT_EQ((std::vector<SQLHDESC>{BaseImplicit(fetch_stmt, SQL_ATTR_APP_ROW_DESC),
                                     BaseImplicit(insert_stmt, SQL_ATTR_APP_PARAM_DESC)}),
              desc_base_driver->bound);
    EXPECT_EQ(0, desc_base_driver->foreign);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, insert_stmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, fetch_stmt));
    Disconnect(dbc);
}

#if UNICODE && !defined(_WIN32)
// The wrapper's copy of a SQL_C_WCHAR binding lives on the descriptor that holds the binding, which the base driver's
// record points at, not on the statement that made it. So an explicit ARD keeps its copies when the statement that
// bound through it goes, and another statement fetches into them. They were the statement's: the base driver wrote
// the second statement's row into the freed copy (ASan: heap-use-after-free) and the app's buffer got nothing.
TEST_F(DescriptorTest, ExplicitArdKeepsItsCopiesWhenTheStatementThatBoundGoes) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT binding_stmt = AllocStmt(dbc);
        SQLHSTMT fetching_stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        AppBuffer buffer;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(binding_stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(binding_stmt, 1, buffer));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, binding_stmt));

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(fetching_stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(fetching_stmt));

        EXPECT_EQ("abc", buffer.Text(four_byte_app));
        EXPECT_EQ(AbcLength(four_byte_app), buffer.indicator);
        EXPECT_EQ(0, desc_base_driver->foreign);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, fetching_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
        Disconnect(dbc);
    }
}

// A column bound again while an explicit ARD is attached is bound in that one: the implicit ARD keeps its copy, which
// its base record points at, and a fetch after the revert fills the buffer bound through it. The rebind replaced the
// statement's one copy, so the base driver wrote the fetch into the freed copy and the other buffer got the value.
TEST_F(DescriptorTest, RebindOnAnExplicitArdLeavesTheImplicitArdsCopy) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        AppBuffer implicit_buffer;
        AppBuffer explicit_buffer;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, implicit_buffer));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, explicit_buffer));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));

        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ("abc", implicit_buffer.Text(four_byte_app));
        EXPECT_EQ(AbcLength(four_byte_app), implicit_buffer.indicator);
        EXPECT_TRUE(explicit_buffer.Untouched());

        // And back on the explicit one, its own copy.
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        desc_base_driver->column_values = {{1, "de"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ("de", explicit_buffer.Text(four_byte_app));
        EXPECT_EQ("abc", implicit_buffer.Text(four_byte_app));
        EXPECT_EQ(0, desc_base_driver->foreign);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
        Disconnect(dbc);
    }
}

// A fetch converts the copies of the ARD in use only. With an explicit ARD attached it converted the implicit ARD's
// copies too (the base driver didn't write them) and wrote them into the buffers bound only through that one.
TEST_F(DescriptorTest, FetchConvertsOnlyTheCopiesOfTheArdInUse) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        AppBuffer implicit_buffer;
        AppBuffer explicit_buffer;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, implicit_buffer));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 2, explicit_buffer));

        desc_base_driver->column_values = {{1, "abc"}, {2, "de"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));

        EXPECT_EQ("de", explicit_buffer.Text(four_byte_app));
        EXPECT_EQ(four_byte_app ? 8 : 4, explicit_buffer.indicator);
        EXPECT_TRUE(implicit_buffer.Untouched());
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
        Disconnect(dbc);
    }
}

// Freeing the explicit ARD puts the statement back on the implicit one's copies, and the explicit one's go with it:
// nothing is written into the buffers bound only through it. They stayed on the statement, and each fetch wrote them.
TEST_F(DescriptorTest, FreeingTheExplicitArdGoesBackToTheImplicitArdsCopies) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        AppBuffer implicit_buffer;
        AppBuffer explicit_buffer;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, implicit_buffer));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 2, explicit_buffer));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));

        desc_base_driver->column_values = {{1, "abc"}, {2, "de"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));

        EXPECT_EQ("abc", implicit_buffer.Text(four_byte_app));
        EXPECT_EQ(AbcLength(four_byte_app), implicit_buffer.indicator);
        EXPECT_TRUE(explicit_buffer.Untouched());
        EXPECT_EQ(0, desc_base_driver->foreign);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The rebind before an execute rebinds the copies of the APD in use only, so the records the app set in an explicit
// APD stay as they are, and the implicit APD's copy is there again after the revert. It rebound the statement's copy
// into whichever APD was current, over the explicit APD's record.
TEST_F(DescriptorTest, ExecuteRebindLeavesAnExplicitApdsRecords) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC apd = AllocDesc(dbc);
        AppBuffer text;
        text.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, text));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, apd, SQL_IS_POINTER));
        SQLINTEGER number = 42;
        SQLLEN number_length = 0;
        EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &number, 0,
                                                &number_length));

        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("42", desc_base_driver->executed.back()[1]);
        EXPECT_EQ(static_cast<SQLPOINTER>(&number), desc_base_driver->records[AsDesc(apd)->wrapped_desc][1].data);

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt));
        ASSERT_EQ(2U, desc_base_driver->executed.size());
        EXPECT_EQ("in", desc_base_driver->executed.back()[1]);
        EXPECT_EQ(0, desc_base_driver->foreign);
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, apd));
        Disconnect(dbc);
    }
}

// One descriptor as one statement's ARD and another's APD (the ODBC reference's way to copy rows), and then as a
// third statement's ARD. The base driver reads the fetched row in its own width from the descriptor's copy, also
// after the statement that bound goes, and each fetch through it fills the app's buffer once. The copy was converted
// in place to UTF-16, so a 4-byte base driver read garbage; it was freed with the first statement; and the third
// statement's fetch converted nothing.
TEST_F(DescriptorTest, SharedDescriptorKeepsTheBaseDriversValueForTheOtherStatements) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT fetch_stmt = AllocStmt(dbc);
        SQLHSTMT insert_stmt = AllocStmt(dbc);
        SQLHSTMT other_fetch_stmt = AllocStmt(dbc);
        SQLHDESC shared = AllocDesc(dbc);
        AppBuffer buffer;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(fetch_stmt, SQL_ATTR_APP_ROW_DESC, shared, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(insert_stmt, SQL_ATTR_APP_PARAM_DESC, shared, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(fetch_stmt, 1, buffer));

        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(fetch_stmt));
        EXPECT_EQ("abc", buffer.Text(four_byte_app));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(insert_stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("abc", desc_base_driver->executed.back()[1]);

        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, fetch_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(insert_stmt));
        ASSERT_EQ(2U, desc_base_driver->executed.size());
        EXPECT_EQ("abc", desc_base_driver->executed.back()[1]);

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(other_fetch_stmt, SQL_ATTR_APP_ROW_DESC, shared, SQL_IS_POINTER));
        desc_base_driver->column_values = {{1, "de"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(other_fetch_stmt));
        EXPECT_EQ("de", buffer.Text(four_byte_app));
        EXPECT_EQ(four_byte_app ? 8 : 4, buffer.indicator);
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(insert_stmt));
        ASSERT_EQ(3U, desc_base_driver->executed.size());
        EXPECT_EQ("de", desc_base_driver->executed.back()[1]);
        EXPECT_EQ(0, desc_base_driver->foreign);
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other_fetch_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, insert_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, shared));
        Disconnect(dbc);
    }
}

// SQL_UNBIND and SQL_RESET_PARAMS drop the copies of the ARD and APD in use, which the base driver emptied, and leave
// the implicit ones' (their base records still point at them). They dropped all of the statement's copies, so after
// the revert the base driver wrote and read freed copies.
TEST_F(DescriptorTest, UnbindAndResetParamsDropOnlyTheCopiesOfTheDescriptorsInUse) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = AllocDesc(dbc);
        SQLHDESC apd = AllocDesc(dbc);
        AppBuffer implicit_column;
        AppBuffer implicit_parameter;
        AppBuffer explicit_column;
        AppBuffer explicit_parameter;
        implicit_parameter.Set("in", four_byte_app);
        explicit_parameter.Set("other", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, implicit_column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, implicit_parameter));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, apd, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, explicit_column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, explicit_parameter));

        EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_RESET_PARAMS));
        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_TRUE(explicit_column.Untouched());

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, SQL_NULL_HDESC, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ("abc", implicit_column.Text(four_byte_app));
        EXPECT_EQ(AbcLength(four_byte_app), implicit_column.indicator);
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("in", desc_base_driver->executed.back()[1]);
        EXPECT_EQ(0, desc_base_driver->foreign);
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, ard));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, apd));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// SQLCopyDesc of an ARD or APD with copies gives the target copies of its own, which its base records point at: the
// source's statement can go, and a fetch or an execute through the target uses the app's buffers. The target's
// records pointed at the source's copies (psqlODBC copies the buffer pointers), which went with the source's statement.
TEST_F(DescriptorTest, CopyDescGivesTheTargetCopiesOfItsOwn) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT source_stmt = AllocStmt(dbc);
        SQLHSTMT target_stmt = AllocStmt(dbc);
        SQLHDESC target_ard = AllocDesc(dbc);
        SQLHDESC target_apd = AllocDesc(dbc);
        AppBuffer column;
        AppBuffer parameter;
        parameter.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(source_stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(source_stmt, parameter));

        EXPECT_EQ(SQL_SUCCESS, SQLCopyDesc(GetDesc(source_stmt, SQL_ATTR_APP_ROW_DESC), target_ard));
        EXPECT_EQ(SQL_SUCCESS, SQLCopyDesc(GetDesc(source_stmt, SQL_ATTR_APP_PARAM_DESC), target_apd));
        for (const auto& [attribute, target] : {std::pair{SQL_ATTR_APP_ROW_DESC, target_ard},
                                                std::pair{SQL_ATTR_APP_PARAM_DESC, target_apd}}) {
            const FakeRecord source_record = desc_base_driver->records[BaseImplicit(source_stmt, attribute)][1];
            const FakeRecord target_record = desc_base_driver->records[AsDesc(target)->wrapped_desc][1];
            EXPECT_NE(source_record.data, target_record.data) << attribute;
            EXPECT_NE(source_record.indicator, target_record.indicator) << attribute;
        }
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, source_stmt));

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(target_stmt, SQL_ATTR_APP_ROW_DESC, target_ard, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(target_stmt, SQL_ATTR_APP_PARAM_DESC, target_apd, SQL_IS_POINTER));
        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(target_stmt));
        EXPECT_EQ("abc", column.Text(four_byte_app));
        EXPECT_EQ(AbcLength(four_byte_app), column.indicator);
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(target_stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("in", desc_base_driver->executed.back()[1]);
        EXPECT_EQ(0, desc_base_driver->foreign);
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, target_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, target_ard));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, target_apd));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// A descriptor that is one statement's ARD and another's APD has one set of records (psqlODBC keeps the ARD and APD
// fields of a descriptor in one union, "APD must be of the same format as ARD"). So a column bound in record 1 takes
// the place of the parameter bound there, and SQL_UNBIND or SQL_RESET_PARAMS through either statement empties all of
// its records: the copies of what's gone go too. The parameter's copy stayed, and the rebind before each execute put
// it back over the column's binding, or after the unbind.
TEST_F(DescriptorTest, ColumnAndParameterOfASharedDescriptorShareItsRecords) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT fetch_stmt = AllocStmt(dbc);
        SQLHSTMT insert_stmt = AllocStmt(dbc);
        SQLHDESC shared = AllocDesc(dbc);
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(fetch_stmt, SQL_ATTR_APP_ROW_DESC, shared, SQL_IS_POINTER));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(insert_stmt, SQL_ATTR_APP_PARAM_DESC, shared, SQL_IS_POINTER));
        AppBuffer parameter;
        AppBuffer column;
        parameter.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(insert_stmt, parameter));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(fetch_stmt, 1, column));

        desc_base_driver->column_values = {{1, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(fetch_stmt));
        EXPECT_EQ("abc", column.Text(four_byte_app));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(insert_stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("abc", desc_base_driver->executed.back()[1]);

        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(insert_stmt, parameter));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(fetch_stmt, SQL_UNBIND));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(insert_stmt));
        ASSERT_EQ(2U, desc_base_driver->executed.size());
        EXPECT_FALSE(desc_base_driver->executed.back().contains(1));
        EXPECT_EQ(0, desc_base_driver->foreign);
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, fetch_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, insert_stmt));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, shared));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// Every SQLBindCol drops the column's copy once the base driver took the new binding, whatever it is: another type,
// SQL_C_WCHAR with a BufferLength of 0, or an unbind. Only an intercepted binding gets a new copy. A rebind that isn't
// intercepted left the copy, and each fetch wrote its old value into the app's old buffer.
TEST_F(DescriptorTest, EveryBindColumnDropsTheColumnsCopy) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        const DESC* implicit_ard = AsDesc(GetDesc(stmt, SQL_ATTR_APP_ROW_DESC));
        SQLINTEGER number = 0;
        AppBuffer other;
        struct Rebind {
            const char* name;
            SQLSMALLINT type;
            SQLPOINTER value;
            SQLLEN length;
        };
        for (const Rebind& rebind : {Rebind{"SQL_C_SLONG", SQL_C_SLONG, &number, 0},
                                     Rebind{"SQL_C_WCHAR, BufferLength 0", SQL_C_WCHAR, other.Get(), 0},
                                     Rebind{"unbind", SQL_C_WCHAR, nullptr, 0}}) {
            SCOPED_TRACE(rebind.name);
            AppBuffer old_buffer;
            EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, old_buffer));
            ASSERT_EQ(1U, implicit_ard->bound_col_buffers.size());
            desc_base_driver->column_values = {{1, "abc"}};
            EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
            EXPECT_EQ("abc", old_buffer.Text(four_byte_app));
            old_buffer.Reset();

            EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, rebind.type, rebind.value, rebind.length, &other.indicator));
            EXPECT_TRUE(implicit_ard->bound_col_buffers.empty());
            EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
            EXPECT_TRUE(old_buffer.Untouched());
        }
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// Every SQLBindParameter does the same with the parameter's copy: rebound to SQL_C_SLONG, the execute reads the new
// binding, and a SQL_C_WCHAR one with a NULL buffer leaves no copy either. The copy stayed, and the rebind before each
// execute put it back over the app's new binding.
TEST_F(DescriptorTest, EveryBindParameterDropsTheParametersCopy) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT stmt = AllocStmt(dbc);
        const DESC* implicit_apd = AsDesc(GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC));
        AppBuffer text;
        text.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, text));
        ASSERT_EQ(1U, implicit_apd->bound_param_buffers.size());
        SQLINTEGER number = 42;
        SQLLEN number_length = 0;
        EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &number, 0,
                                                &number_length));
        EXPECT_TRUE(implicit_apd->bound_param_buffers.empty());
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_EQ("42", desc_base_driver->executed.back()[1]);

        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, text));
        ASSERT_EQ(1U, implicit_apd->bound_param_buffers.size());
        SQLLEN null_indicator = SQL_NULL_DATA;
        EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, nullptr, 0,
                                                &null_indicator));
        EXPECT_TRUE(implicit_apd->bound_param_buffers.empty());
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The control: with both SQLWCHARs the same width nothing is copied, and the base driver gets the app's buffers as they
// are, before and after a rebind.
TEST_F(DescriptorTest, MatchedWidthsBindTheAppsBuffersAsTheyAre) {
    for (const bool four_byte : {false, true}) {
        SCOPED_TRACE(four_byte ? "both 4 bytes" : "both 2 bytes");
        UseMatchedWidths(four_byte);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        const DESC* implicit_ard = AsDesc(GetDesc(stmt, SQL_ATTR_APP_ROW_DESC));
        const DESC* implicit_apd = AsDesc(GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC));
        AppBuffer column;
        AppBuffer parameter;
        parameter.Set("in", four_byte);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, parameter));
        EXPECT_TRUE(implicit_ard->bound_col_buffers.empty());
        EXPECT_TRUE(implicit_apd->bound_param_buffers.empty());
        const FakeRecord column_record = desc_base_driver->records[implicit_ard->wrapped_desc][1];
        EXPECT_EQ(column.Get(), column_record.data);
        EXPECT_EQ(&column.indicator, column_record.indicator);
        EXPECT_EQ(AppBuffer::BYTES, column_record.octet_length);
        EXPECT_EQ(parameter.Get(), desc_base_driver->records[implicit_apd->wrapped_desc][1].data);

        SQLINTEGER number = 0;
        EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_SLONG, &number, 0, &column.indicator));
        EXPECT_EQ(static_cast<SQLPOINTER>(&number), desc_base_driver->records[implicit_ard->wrapped_desc][1].data);
        EXPECT_TRUE(implicit_ard->bound_col_buffers.empty());
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
namespace {
    // A statement attribute that has the base driver use a binding for more rows or parameter sets, row-wise or at an
    // offset, with a value that does and the one that keeps one row by column; and whether it's the ARD's (columns) or
    // the APD's (parameters).
    struct MultiRowSetting {
        const char* name;
        SQLINTEGER attribute;
        SQLULEN value;
        SQLULEN one_row;
        bool rows;
    };

    std::vector<MultiRowSetting> MultiRowSettings(SQLULEN* offset) {
        const auto offset_value = reinterpret_cast<SQLULEN>(offset);
        return {{"SQL_ATTR_ROW_ARRAY_SIZE", SQL_ATTR_ROW_ARRAY_SIZE, 2, 1, true},
                {"SQL_ROWSET_SIZE", SQL_ROWSET_SIZE, 2, 1, true},
                {"SQL_ATTR_ROW_BIND_TYPE", SQL_ATTR_ROW_BIND_TYPE, 16, SQL_BIND_BY_COLUMN, true},
                {"SQL_ATTR_ROW_BIND_OFFSET_PTR", SQL_ATTR_ROW_BIND_OFFSET_PTR, offset_value, 0, true},
                {"SQL_ATTR_PARAMSET_SIZE", SQL_ATTR_PARAMSET_SIZE, 2, 1, false},
                {"SQL_ATTR_PARAM_BIND_TYPE", SQL_ATTR_PARAM_BIND_TYPE, 16, SQL_PARAM_BIND_BY_COLUMN, false},
                {"SQL_ATTR_PARAM_BIND_OFFSET_PTR", SQL_ATTR_PARAM_BIND_OFFSET_PTR, offset_value, 0, false}};
    }
}  // namespace

// A copy of a SQL_C_WCHAR binding holds one row or parameter set at the binding's own address, so while there are
// copies an array size above 1, row-wise binding or a bind offset is SQL_ERROR with HYC00, and the base driver doesn't
// get it: through a statement attribute, the ODBC 2 functions, or the descriptor's header fields. One row by column
// stays fine. The base driver got them and wrote rows 2 and up, and their lengths, past the copies (ASan:
// heap-buffer-overflow).
TEST_F(DescriptorTest, ArraysRowWiseBindingAndBindOffsetsAreRefusedWhileThereAreCopies) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        desc_base_driver->stmt_attr_sets.clear();
        desc_base_driver->desc_field_sets.clear();
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
        SQLHDESC apd = GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC);
        AppBuffer column;
        AppBuffer parameter;
        parameter.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, parameter));
        SQLULEN offset = 0;

        for (const MultiRowSetting& setting : MultiRowSettings(&offset)) {
            EXPECT_EQ(SQL_ERROR, RDS_SQLSetStmtAttr(stmt, setting.attribute, reinterpret_cast<SQLPOINTER>(setting.value),
                                                    0)) << setting.name;
            EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err)) << setting.name;
        }
        EXPECT_TRUE(desc_base_driver->stmt_attr_sets.empty());
        for (const MultiRowSetting& setting : MultiRowSettings(&offset)) {
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, setting.attribute,
                                                      reinterpret_cast<SQLPOINTER>(setting.one_row), 0))
                << setting.name;
        }
        EXPECT_EQ(MultiRowSettings(&offset).size(), desc_base_driver->stmt_attr_sets.size());

        SQLULEN processed = 0;
        EXPECT_EQ(SQL_ERROR, SQLSetStmtOption(stmt, SQL_ROWSET_SIZE, 2));
        EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err));
        EXPECT_EQ(SQL_ERROR, SQLParamOptions(stmt, 2, &processed));
        EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err));
        EXPECT_EQ(SQL_ERROR, SQLSetScrollOptions(stmt, SQL_CONCUR_READ_ONLY, SQL_SCROLL_STATIC, 2));
        EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err));
        EXPECT_EQ(0, desc_base_driver->odbc2_calls);

        for (SQLHDESC desc : {ard, apd}) {
            for (const auto& [field, value] : {std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_ARRAY_SIZE,
                                                                                   reinterpret_cast<SQLPOINTER>(2)},
                                               std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_BIND_TYPE,
                                                                                   reinterpret_cast<SQLPOINTER>(16)},
                                               std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_BIND_OFFSET_PTR, &offset}}) {
                EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescField(desc, 0, field, value, 0)) << field;
                EXPECT_EQ("HYC00", StateOf(AsDesc(desc)->err)) << field;
            }
        }
        EXPECT_TRUE(desc_base_driver->desc_field_sets.empty());
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The other way round: while one of those is set (also through a descriptor's header field), a binding that would get
// a copy is SQL_ERROR with HYC00 and reaches neither the base driver nor the copies; one the wrapper doesn't copy is
// fine.
TEST_F(DescriptorTest, ACopiedBindingIsRefusedWhileArraysRowWiseBindingOrBindOffsetsAreSet) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
        SQLHDESC apd = GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC);
        SQLULEN offset = 0;
        SQLINTEGER number = 0;
        SQLLEN number_length = 0;

        for (const MultiRowSetting& setting : MultiRowSettings(&offset)) {
            SCOPED_TRACE(setting.name);
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, setting.attribute, reinterpret_cast<SQLPOINTER>(setting.value),
                                                      0));
            AppBuffer buffer;
            buffer.Set("in", four_byte_app);
            const size_t bound = desc_base_driver->bound.size();
            EXPECT_EQ(SQL_ERROR, setting.rows ? BindTextColumn(stmt, 1, buffer) : BindTextParameter(stmt, buffer));
            EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err));
            EXPECT_EQ(bound, desc_base_driver->bound.size());
            EXPECT_TRUE(AsDesc(ard)->bound_col_buffers.empty());
            EXPECT_TRUE(AsDesc(apd)->bound_param_buffers.empty());
            EXPECT_EQ(SQL_SUCCESS, setting.rows
                ? SQLBindCol(stmt, 1, SQL_C_SLONG, &number, 0, &number_length)
                : SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &number, 0, &number_length));
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, setting.attribute,
                                                      reinterpret_cast<SQLPOINTER>(setting.one_row), 0));
        }

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 0, SQL_DESC_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(5), 0));
        AppBuffer column;
        EXPECT_EQ(SQL_ERROR, BindTextColumn(stmt, 1, column));
        EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err));
        EXPECT_TRUE(AsDesc(ard)->bound_col_buffers.empty());
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The control: with both SQLWCHARs the same width there are no copies, and arrays, row-wise binding and offsets go to
// the base driver with SQL_C_WCHAR bindings, before and after them.
TEST_F(DescriptorTest, MatchedWidthsTakeArraysRowWiseBindingAndBindOffsets) {
    for (const bool four_byte : {false, true}) {
        SCOPED_TRACE(four_byte ? "both 4 bytes" : "both 2 bytes");
        UseMatchedWidths(four_byte);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        AppBuffer column;
        AppBuffer parameter;
        parameter.Set("in", four_byte);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, parameter));
        SQLULEN offset = 0;
        for (const MultiRowSetting& setting : MultiRowSettings(&offset)) {
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetStmtAttr(stmt, setting.attribute, reinterpret_cast<SQLPOINTER>(setting.value), 0))
                << setting.name;
        }
        EXPECT_EQ(MultiRowSettings(&offset).size(), desc_base_driver->stmt_attr_sets.size());
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, parameter));
        EXPECT_EQ(SQL_SUCCESS, SQLSetStmtOption(stmt, SQL_ROWSET_SIZE, 2));
        EXPECT_EQ(1, desc_base_driver->odbc2_calls);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// SQLSetPos(SQL_UPDATE, SQL_ADD, SQL_REFRESH) and SQLBulkOperations, while the statement's ARD has copies of
// SQL_C_WCHAR columns, are SQL_ERROR with HYC00 and don't reach the base driver: it would read the row from the copies
// (the last fetch, in its own width, not the app's edits) and a refresh would write them, not the app's buffers.
// SQL_POSITION and SQL_DELETE don't use the bindings, and without copies all of it goes through.
TEST_F(DescriptorTest, SetPosAndBulkOperationsThroughCopiesAreRefused) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        desc_base_driver->set_pos_operations.clear();
        desc_base_driver->bulk_operations.clear();
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        AppBuffer column;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));

        for (const SQLUSMALLINT operation : {SQL_UPDATE, SQL_ADD, SQL_REFRESH}) {
            EXPECT_EQ(SQL_ERROR, SQLSetPos(stmt, 1, operation, SQL_LOCK_NO_CHANGE)) << operation;
            EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err)) << operation;
        }
        for (const SQLSMALLINT operation : {SQL_ADD, SQL_UPDATE_BY_BOOKMARK, SQL_DELETE_BY_BOOKMARK,
                                            SQL_FETCH_BY_BOOKMARK}) {
            EXPECT_EQ(SQL_ERROR, SQLBulkOperations(stmt, operation)) << operation;
            EXPECT_EQ("HYC00", StateOf(AsStmt(stmt)->err)) << operation;
        }
        EXPECT_TRUE(desc_base_driver->set_pos_operations.empty());
        EXPECT_TRUE(desc_base_driver->bulk_operations.empty());

        EXPECT_EQ(SQL_SUCCESS, SQLSetPos(stmt, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE));
        EXPECT_EQ(SQL_SUCCESS, SQLSetPos(stmt, 1, SQL_DELETE, SQL_LOCK_NO_CHANGE));
        SQLINTEGER number = 0;
        SQLLEN number_length = 0;
        EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_SLONG, &number, 0, &number_length));
        EXPECT_EQ(SQL_SUCCESS, SQLSetPos(stmt, 1, SQL_UPDATE, SQL_LOCK_NO_CHANGE));
        EXPECT_EQ(SQL_SUCCESS, SQLBulkOperations(stmt, SQL_ADD));
        EXPECT_EQ((std::vector<SQLUSMALLINT>{SQL_POSITION, SQL_DELETE, SQL_UPDATE}), desc_base_driver->set_pos_operations);
        EXPECT_EQ(std::vector<SQLSMALLINT>{SQL_ADD}, desc_base_driver->bulk_operations);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The control: with both SQLWCHARs the same width the base driver gets them with SQL_C_WCHAR columns too.
TEST_F(DescriptorTest, MatchedWidthsTakeSetPosAndBulkOperations) {
    for (const bool four_byte : {false, true}) {
        SCOPED_TRACE(four_byte ? "both 4 bytes" : "both 2 bytes");
        UseMatchedWidths(four_byte);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        AppBuffer column;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, column));
        EXPECT_EQ(SQL_SUCCESS, SQLSetPos(stmt, 1, SQL_UPDATE, SQL_LOCK_NO_CHANGE));
        EXPECT_EQ(SQL_SUCCESS, SQLSetPos(stmt, 1, SQL_REFRESH, SQL_LOCK_NO_CHANGE));
        EXPECT_EQ(SQL_SUCCESS, SQLBulkOperations(stmt, SQL_ADD));
        EXPECT_EQ((std::vector<SQLUSMALLINT>{SQL_UPDATE, SQL_REFRESH}), desc_base_driver->set_pos_operations);
        EXPECT_EQ(std::vector<SQLSMALLINT>{SQL_ADD}, desc_base_driver->bulk_operations);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
namespace {
    SQLPOINTER AsFieldValue(SQLLEN value) {
        return reinterpret_cast<SQLPOINTER>(value);
    }
}  // namespace

// SQLSetDescField and SQLSetDescRec on an ARD or APD record, when the app's and the base driver's SQLWCHAR differ: a
// record made SQL_C_WCHAR, or a SQL_C_WCHAR one given a buffer, length or indicator, is SQL_ERROR with HYC00 and doesn't
// reach the base driver, which would read and write its own width in the app's buffer; so is a binding field of a
// record SQLBindCol or SQLBindParameter copied, whose base record points at the copy. Bindings go through those. Other
// types, records and fields, and the IRD, go through as before.
TEST_F(DescriptorTest, WideRecordsThroughTheDescriptorAreRefusedWhenWidthsDiffer) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        desc_base_driver->desc_field_sets.clear();
        desc_base_driver->desc_rec_sets = 0;
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
        SQLHDESC apd = GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC);
        SQLHDESC ird = GetDesc(stmt, SQL_ATTR_IMP_ROW_DESC);
        AppBuffer buffer;
        SQLLEN length = 0;
        SQLINTEGER number = 0;

        for (SQLHDESC desc : {ard, apd}) {
            for (const SQLSMALLINT field : {SQL_DESC_TYPE, SQL_DESC_CONCISE_TYPE}) {
                EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescField(desc, 1, field, AsFieldValue(SQL_C_WCHAR), 0)) << field;
                EXPECT_EQ("HYC00", StateOf(AsDesc(desc)->err)) << field;
            }
            EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescRec(desc, 1, SQL_C_WCHAR, 0, AppBuffer::BYTES, 0, 0, buffer.Get(), &length,
                                                   &length));
            EXPECT_EQ("HYC00", StateOf(AsDesc(desc)->err));
        }
        // A record the base driver has as SQL_C_WCHAR (an SQLCopyDesc from an IRD makes one).
        desc_base_driver->records[AsDesc(ard)->wrapped_desc][2].type = SQL_C_WCHAR;
        for (const auto& [field, value] : {std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_DATA_PTR, buffer.Get()},
                                           std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_OCTET_LENGTH_PTR, &length},
                                           std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_INDICATOR_PTR, &length}}) {
            EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescField(ard, 2, field, value, 0)) << field;
            EXPECT_EQ("HYC00", StateOf(AsDesc(ard)->err)) << field;
        }
        // A record SQLBindCol copied keeps what the bind set.
        AppBuffer column;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 3, column));
        for (const auto& [field, value] : {std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_DATA_PTR, buffer.Get()},
                                           std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_OCTET_LENGTH, AsFieldValue(128)},
                                           std::pair<SQLSMALLINT, SQLPOINTER>{SQL_DESC_TYPE, AsFieldValue(SQL_C_SLONG)}}) {
            EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescField(ard, 3, field, value, 0)) << field;
            EXPECT_EQ("HYC00", StateOf(AsDesc(ard)->err)) << field;
        }
        EXPECT_EQ(SQL_ERROR, RDS_SQLSetDescRec(ard, 3, SQL_C_SLONG, 0, 0, 0, 0, &number, &length, &length));
        EXPECT_EQ("HYC00", StateOf(AsDesc(ard)->err));
        EXPECT_TRUE(desc_base_driver->desc_field_sets.empty());
        EXPECT_EQ(0, desc_base_driver->desc_rec_sets);

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 4, SQL_DESC_CONCISE_TYPE, AsFieldValue(SQL_C_SLONG), 0));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 4, SQL_DESC_DATA_PTR, &number, 0));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 2, SQL_DESC_OCTET_LENGTH, AsFieldValue(64), 0));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescRec(apd, 5, SQL_C_SLONG, 0, 0, 0, 0, &number, &length, &length));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ird, 1, SQL_DESC_CONCISE_TYPE, AsFieldValue(SQL_C_WCHAR), 0));
        EXPECT_EQ(4U, desc_base_driver->desc_field_sets.size());
        EXPECT_EQ(1, desc_base_driver->desc_rec_sets);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}

// The control: with both SQLWCHARs the same width the base driver reads its width in the app's buffer, which is the app's,
// so SQL_C_WCHAR records through the descriptor go through, also on a record SQLBindCol bound (it has no copy then).
TEST_F(DescriptorTest, MatchedWidthsTakeWideRecordsThroughTheDescriptor) {
    for (const bool four_byte : {false, true}) {
        SCOPED_TRACE(four_byte ? "both 4 bytes" : "both 2 bytes");
        UseMatchedWidths(four_byte);
        DBC* dbc = Connect(&base_dbc_);
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
        AppBuffer buffer;
        SQLLEN length = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE, AsFieldValue(SQL_C_WCHAR), 0));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR, buffer.Get(), 0));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescRec(ard, 2, SQL_C_WCHAR, 0, AppBuffer::BYTES, 0, 0, buffer.Get(), &length,
                                                 &length));
        AppBuffer column;
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 3, column));
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 3, SQL_DESC_DATA_PTR, buffer.Get(), 0));
        EXPECT_EQ(3U, desc_base_driver->desc_field_sets.size());
        EXPECT_EQ(1, desc_base_driver->desc_rec_sets);
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// A lower SQL_DESC_COUNT on the ARD or APD drops the copies of the records above it, which the base driver unbound:
// a fetch then leaves the app's old buffer alone (the app may have freed it), and an execute doesn't bind the
// parameter again. A record at or below the count keeps its copy.
TEST_F(DescriptorTest, LowerDescCountDropsTheCopiesAboveIt) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app, 2-byte base" : "2-byte app, 4-byte base");
        UseWidths(four_byte_app);
        DBC* dbc = Connect(&base_dbc_);
        DefaultPlugin plugin(dbc);
        dbc->plugin_head = &plugin;
        SQLHSTMT stmt = AllocStmt(dbc);
        SQLHDESC ard = GetDesc(stmt, SQL_ATTR_APP_ROW_DESC);
        SQLHDESC apd = GetDesc(stmt, SQL_ATTR_APP_PARAM_DESC);
        AppBuffer first;
        AppBuffer second;
        AppBuffer parameter;
        parameter.Set("in", four_byte_app);
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 1, first));
        EXPECT_EQ(SQL_SUCCESS, BindTextColumn(stmt, 2, second));
        EXPECT_EQ(SQL_SUCCESS, BindTextParameter(stmt, parameter));
        desc_base_driver->column_values = {{1, "abc"}, {2, "abc"}};
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ("abc", second.Text(four_byte_app));

        // Column 2 goes, column 1 stays.
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 0, SQL_DESC_COUNT, reinterpret_cast<SQLPOINTER>(1), 0));
        ASSERT_EQ(1U, AsDesc(ard)->bound_col_buffers.size());
        EXPECT_EQ(1, AsDesc(ard)->bound_col_buffers.front().column_number);
        first.Reset();
        second.Reset();
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ("abc", first.Text(four_byte_app));
        EXPECT_TRUE(second.Untouched());

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(ard, 0, SQL_DESC_COUNT, nullptr, 0));
        EXPECT_TRUE(AsDesc(ard)->bound_col_buffers.empty());
        first.Reset();
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_TRUE(first.Untouched());

        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(apd, 0, SQL_DESC_COUNT, nullptr, 0));
        EXPECT_TRUE(AsDesc(apd)->bound_param_buffers.empty());
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt));
        ASSERT_EQ(1U, desc_base_driver->executed.size());
        EXPECT_FALSE(desc_base_driver->executed.back().contains(1));
        desc_base_driver->executed.clear();
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
        Disconnect(dbc);
    }
}
#endif

// SQLCopyDesc(A, B) and SQLCopyDesc(B, A) on two threads finish: each takes both descriptors' locks at once. Taking
// the source's, then the target's, they could deadlock. A deadlock can't be joined, so it ends the test run.
TEST_F(DescriptorTest, CrossedCopiesOnTwoThreadsDontDeadlock) {
    constexpr int COPIES = 2000;
    DBC* dbc = Connect(&base_dbc_);
    SQLHDESC first = AllocDesc(dbc);
    SQLHDESC second = AllocDesc(dbc);
    std::promise<void> first_done;
    std::promise<void> second_done;
    std::future<void> first_finished = first_done.get_future();
    std::future<void> second_finished = second_done.get_future();
    std::thread forward([&] {
        for (int i = 0; i < COPIES; ++i) {
            RDS_SQLCopyDesc(first, second);
        }
        first_done.set_value();
    });
    std::thread backward([&] {
        for (int i = 0; i < COPIES; ++i) {
            RDS_SQLCopyDesc(second, first);
        }
        second_done.set_value();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const bool finished = first_finished.wait_until(deadline) == std::future_status::ready
        && second_finished.wait_until(deadline) == std::future_status::ready;
    if (!finished) {
        ADD_FAILURE() << "SQLCopyDesc deadlocked";
        std::abort();
    }
    forward.join();
    backward.join();
    EXPECT_EQ(2U * COPIES, desc_base_driver->copies.size());
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLCopyDesc(first, first));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, first));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, second));
    Disconnect(dbc);
}

