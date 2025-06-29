#!/bin/bash

# pull_vdocs.sh
# 从vdocs仓库的对应分支对应目录的修改更新到docs对应分支对应目录下

# 配置信息
VDOCS_REPO="github.com:victor1589007281/percona-server.git"
VDOCS_BRANCH="release-8.4.3-3"
VDOCS_DIR="vdocs"

DOCS_REPO="github.com:victor1589007281/docs.git"
DOCS_BRANCH="main"
DOCS_DIR="percona/release-8.4.3-3"

echo "开始从vdocs仓库拉取更新到docs仓库..."
echo "vdocs仓库: $VDOCS_REPO (分支: $VDOCS_BRANCH, 目录: $VDOCS_DIR)"
echo "docs仓库: $DOCS_REPO (分支: $DOCS_BRANCH, 目录: $DOCS_DIR)"

# 使用git subtree pull命令从vdocs仓库拉取更新
git subtree pull --prefix=$DOCS_DIR $VDOCS_REPO $VDOCS_BRANCH --squash

echo "vdocs仓库更新完成！" 