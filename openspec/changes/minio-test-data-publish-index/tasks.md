## 1. MinIO 与输入数据

- [x] 1.1 确认 bucket **`yikv-artifacts`** 存在（必要时运行 **`deploy/scripts/bootstrap_minio.py --namespace yikv --bucket yikv-artifacts`** 或等价 **`mc mb`**）。
- [x] 1.2 将 **`deploy/e2e/fixtures/product/`** 下测试用 Parquet（或文档指定文件）上传到 **`s3://yikv-artifacts/e2e-input/product/`**（与 **`deploy/e2e/examples/publish-spec.product.json`** 中 **`input`** 一致；使用 **`mc cp`** / **`aws s3 cp`** / Console）。
- [x] 1.3 校验 Job 所用 S3 环境变量与 **`run-publish-job.sh`** 一致（**`AWS_ENDPOINT_URL_S3=http://minio.yikv.svc.cluster.local:9000`** 等）。

## 2. publishIndex（pipeline-worker Job）

- [x] 2.1 构建并加载 **`pipeline-worker:latest`**（**`./deploy/e2e/run-publish-job.sh --spec-file deploy/e2e/examples/publish-spec.product.json`** 或手动 **`docker build --target pipeline-worker`** + **`kind load`**）。
- [x] 2.2 提交 Job 并等待成功（脚本加 **`--wait`** 或 **`kubectl wait job/...`**）；从日志或返回中记录 **`build_id`**。
- [x] 2.3 若失败：检查 **`PUBLISH_SPEC_JSON`**、**`schema_json`** 路径、MinIO **`input`** 前缀是否为空或权限错误。

## 3. deployIndex 与在线 db_path

- [x] 3.1 确保 **`pipeline_agent`** 在可写 **`/data/db`** 的环境运行（与 **`yikv_server` Pod 同主机或共享 PVC 的 sidecar；按现网架构选型）。
- [x] 3.2 对 **`POST /deployIndex`** 发送 **`{"table":"product","build_id":"<上一步>"}`**（**`build_id`** 若可省略则按 **`pipeline_ops`** 行为与运维约定执行）。
- [x] 3.3 在 **`yikv_server`** Pod 内 **`ls /data/db/product`**（或等价路径）确认制品落盘；查看服务日志或 **`reload`** 后确认表已加载。

## 4. 文档与固化

- [x] 4.1 将上述步骤沉淀为 **`deploy/e2e/README.md`** 一小节或独立 **`deploy/e2e/MINIO_PUBLISH_DEPLOY.md`**（含前置条件、命令、常见错误）。
- [x] 4.2 （可选）增加 **`deploy/e2e/scripts/upload_product_fixture_to_minio.sh`** 封装 1.2，减少手工路径错误。
