## ADDED Requirements

### Requirement: MinIO 测试输入布局

运维或自动化 SHALL 在对象存储中准备与 **publish-spec** 中 **`input`** 字段一致的键前缀；该前缀 **MUST** 对 **`pipeline-worker` Job** 内配置的 S3 客户端可读（含正确的 bucket、凭证与 **`AWS_ENDPOINT_URL_S3`**）。测试数据 **MUST** 与对应表的 **`schema.json`** 及导入工具约定一致（例如 Parquet 列与类型）。

#### Scenario: Product 表示例

- **WHEN** **`publish-spec`** 声明 **`input`** 为 **`s3://yikv-artifacts/e2e-input/product/`**  
- **THEN** 该前缀下 **MUST** 存在符合 **`deploy/e2e/fixtures/product/schema.json`** 的输入文件，且 **`yikv-artifacts`** bucket 已创建、凭证与 e2e **`server-config.json`** 中 **`artifact_storage`** 一致

### Requirement: publishIndex（Job）成功完成

系统 SHALL 通过 **`PUBLISH_SPEC_JSON`** 触发与 **`POST /publishIndex`** 等价的 **`publish_index`** 流程；Job **MUST** 以零退出码结束，且制品 **MUST** 已写入 **`artifact_storage`** 中约定位置（含 **`build_id`** 可被发现）。

#### Scenario: 使用现有 E2E 脚本提交 Job

- **WHEN** 操作者执行 **`./deploy/e2e/run-publish-job.sh --spec-file deploy/e2e/examples/publish-spec.product.json`**（或等价 **`submit_publish_job.py`** 调用），且 MinIO 输入已就绪  
- **THEN** Kubernetes Job **MUST** 完成且无 **`PUBLISH_SPEC_JSON`**/`PipelineError` 类失败；发布结果 **MUST** 包含可用于 deploy 的 **`build_id`**（由日志或 API 返回体现）

### Requirement: deployIndex 部署到 db_path

在能访问在线 **`db_path=/data/db`** 与对象存储的主机上，**SHALL** 通过 **`POST /deployIndex`**（**`DeployIndexBody`**：`table` 必填，**`build_id`** 按约定可选或必填）完成拉取与 **reload**；执行成功后，**`db_path`** 下 **MUST** 出现对应 **`table`** 的目录结构，且 **`yikv_server` 后续启动或 reload 后 SHALL 能加载该表**（例如启动扫描不再对该表报缺失）。

#### Scenario: 指定 build_id 部署

- **WHEN** **`publishIndex`** 已产生已知 **`build_id`**，且 **`pipeline_agent`** 已指向 **`db_path=/data/db`** 的在线环境  
- **THEN** 对同一 **`table`** 调用 **`deployIndex`** 且传入该 **`build_id`** 后，**SHALL** 返回成功，且在同一 Pod 内检查 **`/data/db/<table>`**（或文档约定的子路径）**MUST** 存在发布产物

### Requirement: 验收与可重复性

变更相关文档或自动化 **MUST** 列出操作顺序：**准备 MinIO 输入 → publishIndex → 记录 build_id → deployIndex → 验证**；同一 **`publish-spec`** 与数据在相同环境下重复执行时，行为 **SHALL** 可预测（失败时 **MUST** 有可操作的错误信息）。

#### Scenario: 操作清单

- **WHEN** 新人按 **`tasks.md`** 中的步骤执行  
- **THEN** 其 **MUST** 能在无额外口头说明的情况下完成从空 **`db_path`** 到至少一张表可加载的闭环，或明确记录阻塞条件（例如缺少 **`pipeline_agent`**）
