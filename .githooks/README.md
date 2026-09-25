# 项目 Git 钩子

本目录保存随仓库分发的 Git 钩子。默认不会自动生效，需要在克隆后执行一次：

```sh
git config core.hooksPath .githooks
```

设置后 `core.hooksPath` 只影响当前仓库，不会影响其他项目。撤销：

```sh
git config --unset core.hooksPath
```

## pre-push

推送前逐个校验本次要推送的提交信息，不符合项目规范就中止推送：

- 首行必须是 `TYPE：中文描述`，分隔符为全角冒号 `：`；
- `TYPE` 取值：`FEAT`、`FIX`、`STYLE`、`VER`、`DOCS`、`REFACTOR`、`PERF`、`TEST`、`CHORE`、`BUILD`、`CI`、`REVERT`；
- `FEAT`、`FIX` 提交还必须包含 `WHY：`、`WHAT：`、`TEST：` 三段正文；
- 合并提交不校验。

示例：

```text
FEAT：恢复网页趋势曲线并改用内置 Canvas 渲染

WHY：Bring-up 阶段为去除公网依赖移除了在线 Chart.js。

WHAT：用页面内置 Canvas 渲染器实现四条趋势曲线。

TEST：ninja -C build 通过。
```

临时跳过校验：`git push --no-verify`。
