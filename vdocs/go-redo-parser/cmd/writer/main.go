package main

import (
"flag"
"fmt"
"os"

"github.com/percona/go-redo-parser/pkg/redolog"
"github.com/percona/go-redo-parser/pkg/types"
)

func main() {
var (
outFile  = flag.String("output", "test_redo.log", "Output redo log file")
startLsn = flag.Uint64("start-lsn", 8192, "Starting LSN")
)

flag.Parse()

fmt.Printf("Creating redo log file: %s\n", *outFile)
fmt.Printf("Starting LSN: %d\n", *startLsn)

writer, err := redolog.NewWriter(*outFile, types.LSN(*startLsn))
if err != nil {
fmt.Printf("Error creating writer: %v\n", err)
os.Exit(1)
}
defer writer.Close()

// Create sample MTR 1: Write 1 byte
mtr1 := &types.MTR{
StartLsn: writer.GetCurrentLSN(),
Records: []*types.LogRecord{
{
Type:    types.Mlog1Byte | types.MlogSingleRecFlag,
SpaceID: 0,
PageNo:  7,
Offset:  100,
Data:    []byte{0x42},
},
},
}

fmt.Printf("\nWriting MTR 1: MLOG_1BYTE (single record)\n")
if err := writer.WriteMTR(mtr1); err != nil {
fmt.Printf("Error writing MTR 1: %v\n", err)
os.Exit(1)
}

// Create sample MTR 2: Write 4 bytes
mtr2 := &types.MTR{
StartLsn: writer.GetCurrentLSN(),
Records: []*types.LogRecord{
{
Type:    types.Mlog4Bytes | types.MlogSingleRecFlag,
SpaceID: 1,
PageNo:  10,
Offset:  200,
Data:    []byte{0x12, 0x34, 0x56, 0x78},
},
},
}

fmt.Printf("Writing MTR 2: MLOG_4BYTES (single record)\n")
if err := writer.WriteMTR(mtr2); err != nil {
fmt.Printf("Error writing MTR 2: %v\n", err)
os.Exit(1)
}

// Create sample MTR 3: Write string (multi-record)
mtr3 := &types.MTR{
StartLsn: writer.GetCurrentLSN(),
Records: []*types.LogRecord{
{
Type:    types.MlogWriteString,
SpaceID: 2,
PageNo:  15,
Offset:  50,
Data:    []byte("HELLO WORLD"),
},
{
Type:    types.Mlog2Bytes,
SpaceID: 2,
PageNo:  15,
Offset:  100,
Data:    []byte{0xAB, 0xCD},
},
},
}

fmt.Printf("Writing MTR 3: MLOG_WRITE_STRING + MLOG_2BYTES (multi-record)\n")
if err := writer.WriteMTR(mtr3); err != nil {
fmt.Printf("Error writing MTR 3: %v\n", err)
os.Exit(1)
}

// Create sample MTR 4: Page create
mtr4 := &types.MTR{
StartLsn: writer.GetCurrentLSN(),
Records: []*types.LogRecord{
{
Type:    types.MlogPageCreate | types.MlogSingleRecFlag,
SpaceID: 3,
PageNo:  20,
},
},
}

fmt.Printf("Writing MTR 4: MLOG_PAGE_CREATE (single record)\n")
if err := writer.WriteMTR(mtr4); err != nil {
fmt.Printf("Error writing MTR 4: %v\n", err)
os.Exit(1)
}

// Write some more records to fill multiple blocks
fmt.Printf("\nWriting additional records to fill multiple blocks...\n")
for i := 0; i < 50; i++ {
mtr := &types.MTR{
StartLsn: writer.GetCurrentLSN(),
Records: []*types.LogRecord{
{
Type:    types.Mlog1Byte | types.MlogSingleRecFlag,
SpaceID: types.SpaceID(i),
PageNo:  types.PageNo(100 + i),
Offset:  uint16(i * 10),
Data:    []byte{byte(i)},
},
},
}

if err := writer.WriteMTR(mtr); err != nil {
fmt.Printf("Error writing additional MTR %d: %v\n", i, err)
os.Exit(1)
}
}

finalLsn := writer.GetCurrentLSN()

fmt.Printf("\nRedo log creation complete!\n")
fmt.Printf("Final LSN: %d\n", finalLsn)
fmt.Printf("Total bytes written: %d\n", finalLsn-types.LSN(*startLsn))
fmt.Printf("\nYou can now parse this file using:\n")
fmt.Printf("  go run cmd/parser/main.go -file %s -verbose\n", *outFile)
}
