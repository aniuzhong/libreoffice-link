# soffice loadComponentFromURL 排查摘要 (经验 46)

> **根因**: 相对路径 → `osl::FileBase::getFileURLFromSystemPath` 生成非法 URL (`./test.xlsx` 而非 `file:///...`) → soffice.bin 拒绝。
> **产品代码不受影响**: 调用方 (NovaOfficeCore) 传绝对路径。仅探针传相对路径时触发。
> **治理**: 探针传绝对路径; 新增 `minimal_load_probe` 验证 URP 远程加载基础路径。
> **残留**: pptx URP 远程加载返回 null (原因未明, 不影响 calc 验证)。
