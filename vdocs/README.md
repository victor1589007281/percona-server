# Percona Server Parser Documentation

This directory contains comprehensive documentation for implementing parsers in Go for various Percona Server components.

## Documentation Files

### 1. Binlog Parser
- **Document**: `go-binlog-parser.md`
- **Test SQL**: `binlog-validation-tests.sql`
- **Coverage**: All 43 binlog event types (0-42)
  - FORMAT_DESCRIPTION_EVENT (15)
  - QUERY_EVENT (2)
  - WRITE/UPDATE/DELETE_ROWS_EVENT (30-32)
  - GTID_LOG_EVENT (33)
  - GTID_TAGGED_LOG_EVENT (42) - Latest addition
  - TRANSACTION_PAYLOAD_EVENT (40) - Compression support
  - PARTIAL_UPDATE_ROWS_EVENT (39) - JSON partial updates
  - And 35+ more event types

### 2. Redo Log Parser
- **Document**: `go-redo-parser.md`
- **Test SQL**: `redo-validation-tests.sql`
- **Coverage**: All 77 redo log record types (mlog_id_t)
  - MLOG_1BYTE, MLOG_2BYTES, MLOG_4BYTES, MLOG_8BYTES
  - MLOG_REC_INSERT, MLOG_REC_UPDATE_IN_PLACE, MLOG_REC_DELETE
  - MLOG_PAGE_CREATE, MLOG_COMP_PAGE_CREATE
  - MLOG_UNDO_INSERT, MLOG_UNDO_INIT
  - MLOG_FILE_CREATE, MLOG_FILE_RENAME, MLOG_FILE_DELETE
  - MLOG_ZIP_* compressed page operations
  - MLOG_LOB_* large object operations
  - And 60+ more record types

### 3. Page Parser
- **Document**: `go-page-parser.md`
- **Test SQL**: `page-validation-tests.sql`
- **Coverage**: All 30 InnoDB page types
  - FIL_PAGE_INDEX (17855) - B-Tree index pages
  - FIL_PAGE_RTREE (17854) - Spatial index pages
  - FIL_PAGE_UNDO_LOG (2)
  - FIL_PAGE_TYPE_FSP_HDR (8) - Tablespace header
  - FIL_PAGE_TYPE_BLOB (10) - BLOB pages
  - FIL_PAGE_TYPE_LOB_* (22-29) - New LOB format
  - FIL_PAGE_COMPRESSED (14) - Transparent page compression
  - FIL_PAGE_ENCRYPTED (15) - Encryption support
  - And 20+ more page types

### 4. MySQL Protocol Parser
- **Document**: `go-mysql-protocol-parser.md`
- **Test SQL**: `protocol-validation-tests.sql`
- **Coverage**: All 34 protocol commands
  - COM_QUERY (0x03) - SQL query execution
  - COM_QUIT (0x01) - Connection close
  - COM_INIT_DB (0x02) - Database selection
  - COM_STMT_PREPARE (0x16) - Prepared statements
  - COM_STMT_EXECUTE (0x17) - Execute prepared statement
  - COM_BINLOG_DUMP (0x12) - Replication
  - COM_BINLOG_DUMP_GTID (0x1E) - GTID replication
  - COM_RESET_CONNECTION (0x1F)
  - And 26+ more commands

## Quick Start

### Using the Documentation

1. **Read the Parser Guide**: Start with the `.md` file for the component you want to implement
2. **Review Event/Record Types**: Each document lists all supported types with detailed structures
3. **Study the Examples**: Go code examples show how to parse each format
4. **Run Validation Tests**: Execute the SQL files to generate test data
5. **Parse and Verify**: Use your parser to process the generated data

### Running Validation Tests

#### Binlog Tests
```bash
# Enable binlog
mysql -u root -p -e "SET GLOBAL binlog_format = 'ROW'"

# Run tests
mysql -u root -p < vdocs/binlog-validation-tests.sql

# View generated binlog
mysqlbinlog --verbose --base64-output=DECODE-ROWS /path/to/mysql-bin.000001
```

#### Redo Log Tests
```bash
# Run tests (generates redo log activity)
mysql -u root -p < vdocs/redo-validation-tests.sql

# Redo logs location:
# MySQL 8.0+: datadir/#innodb_redo/
# MySQL 5.7: datadir/ib_logfile*
```

#### Page Tests
```bash
# Run tests (creates various page types)
mysql -u root -p < vdocs/page-validation-tests.sql

# Examine pages
innochecksum /path/to/page_test/*.ibd
ibd2sdi /path/to/page_test/test_index_pages.ibd
```

#### Protocol Tests
```bash
# Capture protocol traffic
tcpdump -i any -w mysql_traffic.pcap -s 65535 port 3306 &

# Run tests
mysql -h 127.0.0.1 -u root -p < vdocs/protocol-validation-tests.sql

# Stop capture (Ctrl+C)
# Analyze with Wireshark or your parser
```

## Implementation Statistics

### Total Coverage
- **Binlog Events**: 43 types
- **Redo Log Records**: 77 types
- **Page Types**: 30 types
- **Protocol Commands**: 34 commands
- **Total Structures**: 184+ distinct data structures

### Key Features Covered

#### Binlog Parser
- ✅ All event versions (V1 obsolete, V2 current)
- ✅ Row-based replication events
- ✅ GTID and GTID-tagged events
- ✅ Compressed transactions (ZSTD)
- ✅ JSON partial updates
- ✅ XA transactions
- ✅ All MySQL column types

#### Redo Log Parser
- ✅ All mlog record types (1-76)
- ✅ Compressed page operations
- ✅ LOB (large object) operations
- ✅ Undo log records
- ✅ File operations (create/rename/delete)
- ✅ Compressed integer format
- ✅ LSN and checkpoint handling

#### Page Parser
- ✅ All page types (0-29)
- ✅ FIL header/trailer parsing
- ✅ Index page structures (B-tree, R-tree)
- ✅ Compressed pages
- ✅ Encrypted pages
- ✅ LOB pages (new format)
- ✅ Checksum verification (CRC32, InnoDB)
- ✅ All row formats (compact/dynamic/compressed)

#### Protocol Parser
- ✅ All protocol commands (0x00-0x21)
- ✅ Handshake phase
- ✅ Authentication (multiple plugins)
- ✅ Text protocol resultsets
- ✅ Binary protocol (prepared statements)
- ✅ Compression support
- ✅ SSL/TLS support
- ✅ Multi-packet messages

## Architecture Notes

### Binlog
- **Format**: Binary log events with common header (19 bytes)
- **Checksum**: CRC32 (4 bytes footer)
- **Versions**: Support for MySQL 5.7+ and 8.0+ formats
- **Special**: GTID_TAGGED_LOG_EVENT (42) is the newest addition

### Redo Log
- **Format**: Mini-transaction records with compressed integers
- **Block Size**: 512 bytes (log blocks)
- **Versions**: Different record IDs for MySQL 8.0.27 (_8027 suffix) vs 8.0.28+
- **Recovery**: Records applied from checkpoint LSN to end

### Pages
- **Size**: 4KB, 8KB, 16KB (default), 32KB, 64KB
- **Header**: FIL header (38 bytes) + page-specific header
- **Trailer**: FIL trailer (8 bytes)
- **Special**: FIL_PAGE_INDEX = 17855 (0x45BF), not sequential

### Protocol
- **Packet**: 3-byte length + 1-byte sequence ID + payload
- **Max Size**: 16MB - 1 byte per packet
- **Byte Order**: Little-endian for all multi-byte integers
- **Versions**: Protocol 4.1 (standard since MySQL 4.1)

## Source Code References

All documentation is based on the current Percona Server codebase:

- `libs/mysql/binlog/event/binlog_event.h` - Binlog events
- `storage/innobase/include/mtr0types.h` - Redo log records
- `storage/innobase/include/fil0fil.h` - Page types
- `include/my_command.h` - Protocol commands

## Contributing

When updating these documents:
1. Check source code for new types/commands
2. Update the corresponding `.md` file
3. Add test cases to the `.sql` file
4. Update this README with new statistics
5. Verify all examples compile and work

## Version Compatibility

These documents cover:
- **MySQL**: 5.7, 8.0, 8.1, 8.2, 8.3, 8.4
- **Percona Server**: 5.7, 8.0, 8.1, 8.2, 8.3, 8.4
- **Protocol Version**: 4.1 (standard)

## License

This documentation is part of Percona Server and follows the same license terms.

## Contact

For questions or issues related to these parser implementations, please refer to the Percona Server documentation or community forums.

---

**Last Updated**: November 24, 2025
**Document Version**: 1.0
**Coverage**: Complete (all current types as of MySQL 8.4 / Percona Server 8.4)
