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
filePath  = flag.String("file", "", "Path to redo log file")
showStats = flag.Bool("stats", false, "Show statistics")
verbose   = flag.Bool("verbose", false, "Verbose output")
startLsn  = flag.Uint64("start-lsn", 0, "Start from specific LSN")
)

flag.Parse()

if *filePath == "" {
fmt.Println("Usage: parser -file <redo-log-file> [-stats] [-verbose] [-start-lsn <lsn>]")
os.Exit(1)
}

reader := redolog.NewReader()
fileParser := redolog.NewFileParser()

// Read file info
fmt.Printf("Reading redo log file: %s\n", *filePath)
info, err := fileParser.ReadFileInfo(*filePath)
if err != nil {
fmt.Printf("Error reading file info: %v\n", err)
os.Exit(1)
}

fmt.Printf("\nFile Information:\n")
fmt.Printf("  File Size: %d bytes\n", info.FileSize)
fmt.Printf("  Format Version: %d\n", info.Header.Format)
fmt.Printf("  Log UUID: 0x%08x\n", info.Header.LogUuid)
fmt.Printf("  Start LSN: %d\n", info.Header.StartLsn)
fmt.Printf("  Creator: %s\n", info.Header.CreatorName)
fmt.Printf("  Flags: 0x%08x\n", info.Header.Flags)

// Read checkpoint
cp1, err := fileParser.ReadCheckpoint(*filePath, 1)
if err == nil {
fmt.Printf("\nCheckpoint 1:\n")
fmt.Printf("  Checkpoint LSN: %d\n", cp1.CheckpointLsn)
}

cp2, err := fileParser.ReadCheckpoint(*filePath, 2)
if err == nil {
fmt.Printf("\nCheckpoint 2:\n")
fmt.Printf("  Checkpoint LSN: %d\n", cp2.CheckpointLsn)
}

// Read and parse records
var mtrs []*types.MTR
var stats *types.RecoveryStats

if *startLsn > 0 {
fmt.Printf("\nReading from LSN %d...\n", *startLsn)
mtrs, err = reader.ReadFromLSN(*filePath, types.LSN(*startLsn))
} else {
fmt.Printf("\nReading entire file...\n")
mtrs, stats, err = reader.ReadFile(*filePath)
}

if err != nil {
fmt.Printf("Error reading redo log: %v\n", err)
os.Exit(1)
}

// Display statistics
if *showStats && stats != nil {
fmt.Printf("\nRecovery Statistics:\n")
fmt.Printf("  Blocks Read: %d\n", stats.BlocksRead)
fmt.Printf("  Records Parsed: %d\n", stats.RecordsParsed)
fmt.Printf("  MTRs Processed: %d\n", stats.MtrsProcessed)
fmt.Printf("  Bytes Processed: %d\n", stats.BytesProcessed)
fmt.Printf("  Corrupt Blocks: %d\n", stats.CorruptBlocks)
}

// Display MTRs
fmt.Printf("\nFound %d MTRs\n", len(mtrs))

if *verbose {
for i, mtr := range mtrs {
if i >= 10 && !*verbose { // Limit output unless verbose
fmt.Printf("... and %d more MTRs\n", len(mtrs)-10)
break
}

fmt.Printf("\nMTR %d: LSN %d-%d (%d records)\n", i+1, mtr.StartLsn, mtr.EndLsn, len(mtr.Records))
for j, record := range mtr.Records {
fmt.Printf("  Record %d: Type=%s LSN=%d", j+1, record.Type.String(), record.LSN)
if record.SpaceID > 0 || record.PageNo > 0 {
fmt.Printf(" Space=%d Page=%d", record.SpaceID, record.PageNo)
}
if record.Offset > 0 {
fmt.Printf(" Offset=%d", record.Offset)
}
if len(record.Data) > 0 && len(record.Data) <= 16 {
fmt.Printf(" Data=%v", record.Data)
} else if len(record.Data) > 0 {
fmt.Printf(" DataLen=%d", len(record.Data))
}
fmt.Println()
}
}
} else if len(mtrs) > 0 {
// Show first few MTRs
for i := 0; i < 5 && i < len(mtrs); i++ {
mtr := mtrs[i]
fmt.Printf("  MTR %d: LSN %d-%d (%d records)\n", i+1, mtr.StartLsn, mtr.EndLsn, len(mtr.Records))
}
if len(mtrs) > 5 {
fmt.Printf("  ... and %d more MTRs (use -verbose to see all)\n", len(mtrs)-5)
}
}
}
