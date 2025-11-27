package main

import (
"flag"
"fmt"
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/fsp"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/checksum"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
var (
ibdFile = flag.String("file", "", "IBD文件路径")
pageNum = flag.Int("page", 0, "要查看的Page号")
verify  = flag.Bool("verify", false, "验证所有Page的Checksum")
)

flag.Parse()

if *ibdFile == "" {
fmt.Println("用法: ibdinfo -file <ibd文件路径> [-page <页号>] [-verify]")
os.Exit(1)
}

// 打开IBD文件
r, err := reader.Open(*ibdFile)
if err != nil {
fmt.Printf("错误: 无法打开文件: %v\n", err)
os.Exit(1)
}
defer r.Close()

fmt.Printf("=== IBD文件信息 ===\n")
fmt.Printf("文件路径: %s\n", *ibdFile)
fmt.Printf("Page大小: %d bytes (%d KB)\n", r.GetPageSize(), r.GetPageSize()/1024)
fmt.Printf("Page总数: %d\n\n", r.GetPageCount())

// 读取并显示FSP Header (Page 0)
page0, err := r.ReadPage(0)
if err != nil {
fmt.Printf("错误: 无法读取Page 0: %v\n", err)
os.Exit(1)
}

fmt.Printf("=== FSP Header (Page 0) ===\n")
fmt.Printf("Page类型: 0x%04X (%s)\n", page0.PageType, getPageTypeName(page0.PageType))
fmt.Printf("Checksum: 0x%08X\n", page0.Checksum)
fmt.Printf("LSN: %d\n", page0.LSN)
fmt.Printf("Space ID: %d\n\n", page0.SpaceID)

if page0.PageType == types.FIL_PAGE_TYPE_FSP_HDR {
fspHeader, err := fsp.ParseFSPHeader(page0)
if err != nil {
fmt.Printf("警告: 解析FSP Header失败: %v\n", err)
} else {
fmt.Printf("FSP SpaceID: %d\n", fspHeader.SpaceID)
fmt.Printf("FSP Size: %d pages\n", fspHeader.Size)
fmt.Printf("FSP FreeLimit: %d\n", fspHeader.FreeLimit)
fmt.Printf("FSP Flags: 0x%08X\n\n", fspHeader.SpaceFlags)
}
}

// 如果指定了Page号，显示该Page信息
if *pageNum > 0 {
page, err := r.ReadPage(uint32(*pageNum))
if err != nil {
fmt.Printf("错误: 无法读取Page %d: %v\n", *pageNum, err)
os.Exit(1)
}

fmt.Printf("=== Page %d 信息 ===\n", *pageNum)
printPageInfo(page)
}

// 验证Checksum
if *verify {
fmt.Printf("\n=== 验证所有Page的Checksum ===\n")
totalPages := r.GetPageCount()
validPages := 0
invalidPages := 0

for i := uint32(0); i < totalPages; i++ {
page, err := r.ReadPage(i)
if err != nil {
continue
}

if checksum.Verify(page.Data, types.ChecksumCRC32) {
validPages++
} else {
invalidPages++
fmt.Printf("警告: Page %d checksum无效\n", i)
}
}

fmt.Printf("\n验证完成:\n")
fmt.Printf("  有效Page: %d\n", validPages)
fmt.Printf("  无效Page: %d\n", invalidPages)
fmt.Printf("  总计: %d\n", totalPages)
}
}

func printPageInfo(page *types.Page) {
fmt.Printf("Page类型: 0x%04X (%s)\n", page.PageType, getPageTypeName(page.PageType))
fmt.Printf("Page号: %d\n", page.PageNumber)
fmt.Printf("Checksum: 0x%08X\n", page.Checksum)
fmt.Printf("LSN: %d\n", page.LSN)
fmt.Printf("Space ID: %d\n", page.SpaceID)
fmt.Printf("前一页: %d\n", page.PrevPage)
fmt.Printf("后一页: %d\n", page.NextPage)
fmt.Printf("Flush LSN: %d\n", page.FlushLSN)
}

func getPageTypeName(pageType uint16) string {
switch pageType {
case types.FIL_PAGE_INDEX:
return "INDEX"
case types.FIL_PAGE_UNDO_LOG:
return "UNDO_LOG"
case types.FIL_PAGE_INODE:
return "INODE"
case types.FIL_PAGE_IBUF_FREE_LIST:
return "IBUF_FREE_LIST"
case types.FIL_PAGE_TYPE_ALLOCATED:
return "ALLOCATED"
case types.FIL_PAGE_IBUF_BITMAP:
return "IBUF_BITMAP"
case types.FIL_PAGE_TYPE_SYS:
return "SYS"
case types.FIL_PAGE_TYPE_TRX_SYS:
return "TRX_SYS"
case types.FIL_PAGE_TYPE_FSP_HDR:
return "FSP_HDR"
case types.FIL_PAGE_TYPE_XDES:
return "XDES"
case types.FIL_PAGE_TYPE_BLOB:
return "BLOB"
case types.FIL_PAGE_SDI:
return "SDI"
case types.FIL_PAGE_TYPE_RSEG_ARRAY:
return "RSEG_ARRAY"
case types.FIL_PAGE_TYPE_LOB_INDEX:
return "LOB_INDEX"
case types.FIL_PAGE_TYPE_LOB_DATA:
return "LOB_DATA"
case types.FIL_PAGE_TYPE_LOB_FIRST:
return "LOB_FIRST"
case types.FIL_PAGE_TYPE_ZLOB_FIRST:
return "ZLOB_FIRST"
case types.FIL_PAGE_TYPE_ZLOB_DATA:
return "ZLOB_DATA"
case types.FIL_PAGE_TYPE_ZLOB_INDEX:
return "ZLOB_INDEX"
case types.FIL_PAGE_TYPE_ZLOB_FRAG:
return "ZLOB_FRAG"
default:
return "UNKNOWN"
}
}
