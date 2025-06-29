#!/bin/bash

# pull_docs.sh
# 从docs对应分支对应目录的修改更新到vdocs仓库的对应分支对应目录下

# 配置信息
VDOCS_REPO="github.com:victor1589007281/percona-server.git"
VDOCS_BRANCH="release-8.4.3-3"
VDOCS_DIR="vdocs"

DOCS_REPO="github.com:victor1589007281/docs.git"
DOCS_BRANCH="main"
DOCS_DIR="percona/release-8.4.3-3"

echo "开始从docs仓库拉取更新到vdocs仓库..."
echo "docs仓库: $DOCS_REPO (分支: $DOCS_BRANCH, 目录: $DOCS_DIR)"
echo "vdocs仓库: $VDOCS_REPO (分支: $VDOCS_BRANCH, 目录: $VDOCS_DIR)"

# 使用git subtree pull命令从docs仓库拉取更新
git subtree pull --prefix=$VDOCS_DIR $DOCS_REPO $DOCS_BRANCH --squash

echo "docs仓库更新完成！" 