# MsQuic Fork 维护策略

`third_party/msquic` 指向项目自己的 MsQuic fork：

```text
git@github.com:jack2007/msquic.git
```

后续同步 MsQuic 上游变更时，按两层仓库处理：

1. 先把 `jack2007/msquic` 从原始上游 `microsoft/msquic` 同步到最新。
2. 再回到 `tcpquic-proxy`，更新 `third_party/msquic` 这个子模块的 gitlink。

## 仓库角色

- `microsoft/msquic`：原始上游仓库。
- `jack2007/msquic`：项目维护的集成 fork，包含上游代码和本项目需要的 MsQuic 修改。
- `tcpquic-proxy/third_party/msquic`：子模块，只记录 `jack2007/msquic` 中某一个具体 commit。

## 推荐同步流程

在子模块目录中执行：

```bash
cd third_party/msquic

git remote -v
git fetch origin
git fetch upstream --tags
git checkout main
git pull --ff-only origin main
git merge --no-ff upstream/main
```

如果有冲突，先解决冲突，再运行相关的 MsQuic 和 `tcpquic-proxy` 验证。验证通过后，把 fork 推送到 GitHub：

```bash
git push origin main
```

然后回到主仓库，更新子模块 pin：

```bash
cd ../..
git add third_party/msquic
git commit -m "chore: update msquic submodule"
git push origin master
```

## 维护原则

- 把 `jack2007/msquic/main` 作为集成分支：上游历史加上项目自己的修改。
- 推荐把 `upstream/main` merge 到 `jack2007/msquic/main`，不要频繁 rebase 公开分支。
- 不要随意 force-push `jack2007/msquic/main`。主仓库的子模块记录的是精确 commit，改写 fork 历史可能导致旧 pin 后续无法 fetch。
- 项目自己的 MsQuic 修改尽量拆成小而清晰的 commit，方便后续和上游同步时处理冲突。
- 只有在 fork 已经推送、验证已经通过之后，才更新 `tcpquic-proxy` 中的 `third_party/msquic` gitlink。

## 什么时候可以 rebase

如果是在私有 topic 分支上整理还没有推送、也没有被 `tcpquic-proxy` pin 住的改动，可以使用 rebase。

一旦 commit 已经推送到 `jack2007/msquic/main`，或者已经被主仓库作为子模块 pin 引用，就避免再 rebase 这段历史。
