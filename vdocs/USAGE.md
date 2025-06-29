# VDocs 使用说明

## 概述
VDocs 是 Percona Server release-8.4.3-3 分支的代码阅读笔记项目，使用 git subtree 进行管理。

## 工作流程

### 1. 添加新的笔记
```bash
# 在vdocs目录中添加或修改文件
cd vdocs/
# 编辑笔记文件...

# 提交到主项目
cd ..
git add vdocs/
git commit -m "更新代码阅读笔记"
```

### 2. 推送到远程仓库（可选）
如果你有独立的docs仓库，可以这样操作：

```bash
# 将vdocs推送到独立的仓库
git subtree push --prefix=vdocs <docs-repo-url> main
```

### 3. 从远程仓库拉取更新
```bash
# 从独立仓库拉取vdocs更新
git subtree pull --prefix=vdocs <docs-repo-url> main --squash
```

## 目录结构说明

- `sql/` - SQL层相关代码分析
- `storage/` - 存储引擎相关代码分析
- `include/` - 头文件分析
- `mysys/` - 系统相关代码分析
- `client/` - 客户端代码分析
- `docs/` - 文档和设计说明

## 笔记格式规范

每个模块的笔记应包含：
1. **概述** - 模块的功能和作用
2. **主要组件** - 关键类和函数说明
3. **设计思路** - 架构设计分析
4. **重要算法** - 核心算法解释
5. **待分析文件** - 待深入研究的文件列表

## 注意事项

- 保持笔记的及时更新
- 使用清晰的目录结构
- 添加适当的交叉引用
- 记录重要的设计决策和实现细节 