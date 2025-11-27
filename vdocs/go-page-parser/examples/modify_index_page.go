package main

import (
"fmt"
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/modifier"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/index"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
if len(os.Args) < 3 {
fmt.Println("用法: modify_index_page <ibd文件路径> <page号>")
os.Exit(1)
}

ibdFile := os.Args[1]
var pageNum uint32
fmt.Sscanf(os.Args[2], "%d", &pageNum)

fmt.Printf("=== Index Page修改示例 ===\n")
fmt.Printf("文件: %s\n", ibdFile)
fmt.Printf("Page号: %d\n\n", pageNum)

// 打开IBD文件
r, err := reader.Open(ibdFile)
if err != nil {
fmt.Printf("错误: 无法打开文件: %v\n", err)
os.Exit(1)
}
defer r.Close()

// 读取Page
page, err := r.ReadPage(pageNum)
if err != nil {
fmt.Printf("错误: 无法读取Page: %v\n", err)
os.Exit(1)
}

// 检查是否为Index Page
if page.PageType != types.FIL_PAGE_INDEX {
fmt.Printf("错误: Page %d不是Index Page (type=0x%04X)\n", pageNum, page.PageType)
os.Exit(1)
}

// 解析Index Page
indexPage, err := index.ParseIndexPage(page)
if err != nil {
fmt.Printf("错误: 解析失败: %v\n", err)
os.Exit(1)
}

fmt.Printf("=== 原始Page信息 ===\n")
printIndexPageInfo(indexPage)

fmt.Printf("\n=== 修改Page ===\n")

// 示例：增加记录数（这只是演示，实际不应该随意修改）
oldNRecs := indexPage.Header.NRecs
newNRecs := oldNRecs + 1

fmt.Printf("修改记录数: %d -> %d\n", oldNRecs, newNRecs)

indexPage.Header.NRecs = newNRecs

// 写回Page Header
index.WriteIndexPageHeader(page, indexPage.Header)

// 更新Checksum
mod := modifier.NewModifier(page)
mod.UpdateChecksum(types.ChecksumCRC32)

fmt.Printf("新Checksum: 0x%08X\n", page.Checksum)

fmt.Printf("\n=== 注意 ===\n")
fmt.Printf("这只是演示程序，实际写入已被禁用\n")
fmt.Printf("要实际写入，请：\n")
fmt.Printf("1. 备份IBD文件\n")
fmt.Printf("2. 停止MySQL\n")
fmt.Printf("3. 取消代码中的写入注释\n")
fmt.Printf("4. 重新编译运行\n")

/*
// 实际写入代码（已注释）
w, err := writer.Open(ibdFile, r.GetPageSize())
if err != nil {
fmt.Printf("错误: 无法打开写入器: %v\n", err)
os.Exit(1)
}
defer w.Close()

if err := w.WritePage(page); err != nil {
fmt.Printf("错误: 写入失败: %v\n", err)
os.Exit(1)
}

if err := w.Sync(); err != nil {
fmt.Printf("错误: 同步失败: %v\n", err)
os.Exit(1)
}

fmt.Printf("\n写入成功！\n")
*/
}

func printIndexPageInfo(indexPage *types.IndexPage) {
fmt.Printf("  Level: %d\n", indexPage.Header.Level)
fmt.Printf("  Records: %d\n", indexPage.Header.NRecs)
fmt.Printf("  Index ID: %d\n", indexPage.Header.IndexID)
fmt.Printf("  Max Trx ID: %d\n", indexPage.Header.MaxTrxID)
fmt.Printf("  Compact Format: %v\n", indexPage.Header.IsCompact)
fmt.Printf("  Directory Slots: %d\n", indexPage.Header.NDirSlots)
fmt.Printf("  Heap Top: %d\n", indexPage.Header.HeapTop)
fmt.Printf("  Garbage: %d bytes\n", indexPage.Header.Garbage)
}
