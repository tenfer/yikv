#pragma once

#include <string>

namespace yikv_server {

class TableRegistry;

// Listens on an AF_UNIX stream socket at `path` (bind after unlink).
// Each connection: one line `reload <table_name>` (optional trailing \\n).
// For an already-open table: swap to new mmap after «active» changed. For a new
// `{db_path}/{table}/`: first open (hot add). Response: `ok\\n` or `err: ...\\n`.
void StartAdminUnixSocket(const std::string& path, TableRegistry* reg);

}  // namespace yikv_server
