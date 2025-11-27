package main

import (
"fmt"
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/factory"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
if len(os.Args) < 2 {
fmt.Println("用法: analyze_all_pages <ibd文件路径>")
os.Exit(1)
}

ibdFile := os.Args[1]

fmt.Printf("=== 分析IBD文件: %s ===\n\n", ibdFile)

// 打开IBD文件
r, err := reader.Open(ibdFile)
if err != nil {
fmt.Printf("错误: 无法打开文件: %v\n", err)
os.Exit(1)
}
defer r.Close()

fmt.Printf("文件信息:\n")
fmt.Printf("  Page大小: %d bytes (%d KB)\n", r.GetPageSize(), r.GetPageSize()/1024)
fmt.Printf("  Page总数: %d\n\n", r.GetPageCount())

// 统计Page类型
pageTypes := make(map[string]int)
pageCount := r.GetPageCount()

// 限制扫描数量（避免大文件耗时太长）
maxScan := pageCount
if maxScan > 1000 {
maxScan = 1000
}

fmt.Printf("=== 扫描前%d个Page ===\n", maxScan)

for i := uint32(0); i < maxScan; i++ {
page, err := r.ReadPage(i)
if err != nil {
continue
}

typeName := factory.GetPageTypeName(page.PageType)
pageTypes[typeName]++

// 详细分析前10个page
if i < 10 {
analyzePageDetail(page, i)
}
}

fmt.Printf("\n=== Page类型统计 ===\n")
for typeName, count := range pageTypes {
percentage := float64(count) / float64(maxScan) * 100
fmt.Printf("  %-20s: %5d pages (%.2f%%)\n", typeName, count, percentage)
}
}

func analyzePageDetail(page *types.Page, pageNum uint32) {
fmt.Printf("\n--- Page %d ---\n", pageNum)
fmt.Printf("类型: %s (0x%04X)\n", factory.GetPageTypeName(page.PageType), page.PageType)
fmt.Printf("LSN: %d\n", page.LSN)
fmt.Printf("Checksum: 0x%08X\n", page.Checksum)

// 根据类型解析详细信息
parsed, err := factory.ParsePage(page)
if err != nil {
fmt.Printf("解析失败: %v\n", err)
return
}

switch page.PageType {
case types.FIL_PAGE_TYPE_FSP_HDR:
var fspHeader *types.FSPHeader
if err := factory.GetSpecificPage(parsed, &fspHeader); err == nil {
fmt.Printf("  Space ID: %d\n", fspHeader.SpaceID)
fmt.Printf("  Size: %d pages\n", fspHeader.Size)
fmt.Printf("  Free Limit: %d\n", fspHeader.FreeLimit)
fmt.Printf("  Flags: 0x%08X\n", fspHeader.SpaceFlags)
}

case types.FIL_PAGE_INDEX:
var indexPage *types.IndexPage
if err := factory.GetSpecificPage(parsed, &indexPage); err == nil {
fmt.Printf("  Level: %d\n", indexPage.Header.Level)
fmt.Printf("  Records: %d\n", indexPage.Header.NRecs)
fmt.Printf("  Index ID: %d\n", indexPage.Header.IndexID)
fmt.Printf("  Compact: %v\n", indexPage.Header.IsCompact)
}

case types.FIL_PAGE_UNDO_LOG:
var undoPage *types.UndoPage
if err := factory.GetSpecificPage(parsed, &undoPage); err == nil {
pageTypeStr := "INSERT"
if undoPage.Header.PageType == uint16(types.TRX_UNDO_UPDATE) {
pageTypeStr = "UPDATE"
}
fmt.Printf("  Undo Type: %s\n", pageTypeStr)
fmt.Printf("  Page Start: %d\n", undoPage.Header.PageStart)
fmt.Printf("  Page Free: %d\n", undoPage.Header.PageFree)
}

case types.FIL_PAGE_INODE:
var inodePage *types.InodePage
if err := factory.GetSpecificPage(parsed, &inodePage); err == nil {
fmt.Printf("  Inode Count: %d\n", len(inodePage.Inodes))
for i, inode := range inodePage.Inodes {
if i < 3 { // 只显示前3个
fmt.Printf("    Inode[%d]: Segment ID=%d\n", i, inode.SegmentID)
}
}
}

case types.FIL_PAGE_TYPE_TRX_SYS:
var trxsysPage *types.TRXSysPage
if err := factory.GetSpecificPage(parsed, &trxsysPage); err == nil {
fmt.Printf("  Trx ID Store: %d\n", trxsysPage.Header.TrxIDStore)
}

case types.FIL_PAGE_SDI:
var sdiPage *types.SDIPage
if err := factory.GetSpecificPage(parsed, &sdiPage); err == nil {
fmt.Printf("  SDI Version: %d\n", sdiPage.Header.Version)
fmt.Printf("  SDI Type: %d\n", sdiPage.Header.Type)
fmt.Printf("  Data Length: %d\n", sdiPage.Header.DataLen)
if sdiPage.Header.DataLen > 0 && len(sdiPage.Data) > 100 {
fmt.Printf("  Data Preview: %s...\n", string(sdiPage.Data[:100]))
}
}
}
}
