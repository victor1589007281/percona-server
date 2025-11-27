package main

import (
"fmt"
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
// "github.com/percona/percona-server/vdocs/go-page-parser/pkg/writer"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/modifier"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
// 示例：修改IBD文件中的Page

ibdFile := "test.ibd"
if len(os.Args) > 1 {
ibdFile = os.Args[1]
}

fmt.Printf("=== Page修改示例 ===\n")
fmt.Printf("文件: %s\n\n", ibdFile)

// 1. 打开IBD文件
r, err := reader.Open(ibdFile)
if err != nil {
fmt.Printf("错误: 无法打开文件: %v\n", err)
return
}
defer r.Close()

// 2. 读取Page 1
page, err := r.ReadPage(1)
if err != nil {
fmt.Printf("错误: 无法读取Page: %v\n", err)
return
}

fmt.Printf("=== 原始Page信息 ===\n")
fmt.Printf("Page号: %d\n", page.PageNumber)
fmt.Printf("Space ID: %d\n", page.SpaceID)
fmt.Printf("Checksum: 0x%08X\n", page.Checksum)
fmt.Printf("前一页: %d\n", page.PrevPage)
fmt.Printf("后一页: %d\n\n", page.NextPage)

// 3. 修改Page
mod := modifier.NewModifier(page)

// 示例：修改Space ID
newSpaceID := page.SpaceID + 1000
mod.SetSpaceID(newSpaceID)
fmt.Printf("修改Space ID: %d -> %d\n", page.SpaceID, newSpaceID)

// 更新Checksum
mod.UpdateChecksum(types.ChecksumCRC32)

modifiedPage := mod.GetPage()
fmt.Printf("新Checksum: 0x%08X\n\n", modifiedPage.Checksum)

// 4. 写回文件（这里只是演示，不实际写入）
fmt.Printf("=== 写回文件（演示模式） ===\n")
fmt.Printf("注意: 这只是演示代码，实际写入已被注释掉\n")
fmt.Printf("要实际写入，请取消以下代码的注释:\n\n")
fmt.Printf("// import \"github.com/percona/percona-server/vdocs/go-page-parser/pkg/writer\"\n")
fmt.Printf("/*\n")
fmt.Printf("w, err := writer.Open(ibdFile, r.GetPageSize())\n")
fmt.Printf("if err != nil {\n")
fmt.Printf("    fmt.Printf(\"错误: 无法打开写入器: %%v\\n\", err)\n")
fmt.Printf("    return\n")
fmt.Printf("}\n")
fmt.Printf("defer w.Close()\n")
fmt.Printf("\n")
fmt.Printf("if err := w.WritePage(modifiedPage); err != nil {\n")
fmt.Printf("    fmt.Printf(\"错误: 写入失败: %%v\\n\", err)\n")
fmt.Printf("    return\n")
fmt.Printf("}\n")
fmt.Printf("\n")
fmt.Printf("if err := w.Sync(); err != nil {\n")
fmt.Printf("    fmt.Printf(\"错误: 同步失败: %%v\\n\", err)\n")
fmt.Printf("    return\n")
fmt.Printf("}\n")
fmt.Printf("*/\n")

// 实际写入代码（已注释）
/*
w, err := writer.Open(ibdFile, r.GetPageSize())
if err != nil {
fmt.Printf("错误: 无法打开写入器: %v\n", err)
return
}
defer w.Close()

if err := w.WritePage(modifiedPage); err != nil {
fmt.Printf("错误: 写入失败: %v\n", err)
return
}

if err := w.Sync(); err != nil {
fmt.Printf("错误: 同步失败: %v\n", err)
return
}

fmt.Printf("\n写入成功！\n")
*/
}
