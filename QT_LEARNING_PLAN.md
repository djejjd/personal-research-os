# Qt 学习方案

这份方案以当前仓库代码为教材，重点理解已经使用的 Qt，而不是泛读所有模块。

## 当前代码中的 Qt 知识点

### 1. 应用对象与事件循环

- `QApplication` 创建桌面应用上下文。
- `QCoreApplication::exec()` 启动事件循环。
- `QTimer::singleShot()` 用于无头冒烟测试中的安全退出。
- `QApplication::arguments()` 获取命令行参数。

对应代码：`app/main.cpp`。

### 2. Widgets 界面基础

- `QMainWindow` 作为主窗口容器。
- `QLabel` 作为最小中央部件。
- `setCentralWidget`、`resize`、`show` 组成最小窗口生命周期。
- Widgets 逻辑应留在表现层，不能绕过 application facade 直接读领域或数据库。

对应代码：`app/main.cpp`、`app/CMakeLists.txt`。

### 3. Qt 类型与数据转换

- `QString` 处理文本和路径。
- `QByteArray` 承载 JSON 序列化结果。
- `QDateTime` 生成 UTC、带毫秒的 ISO 时间。
- `QUuid` 生成进程实例标识。
- `QJsonObject`、`QJsonDocument` 生成紧凑结构化日志。

重点学习 Qt 隐式共享、UTF-8/本地编码边界和 `QString` 与 STL 字符串的转换成本。

### 4. CMake 与 Qt 自动工具

- `find_package(Qt6 ... COMPONENTS Core Widgets Test)` 查找模块。
- `qt_add_executable` 创建 Qt 可执行目标。
- `CMAKE_AUTOMOC`、`CMAKE_AUTOUIC`、`CMAKE_AUTORCC` 自动处理元对象、UI 和资源文件。
- `target_link_libraries` 表达模块依赖，业务层不依赖 Widgets。

对应代码：根 `CMakeLists.txt` 和各模块 `CMakeLists.txt`。

### 5. Qt Test 与无头测试

- `QTest` 驱动测试类和测试槽。
- `QTEST_APPLESS_MAIN` 生成无 GUI 测试入口。
- `QTemporaryDir` 隔离临时数据目录。
- `QT_QPA_PLATFORM=offscreen` 让 CI 在无显示器环境运行应用和 Widgets 测试。

对应代码：`tests/*_test.cpp`、`app/CMakeLists.txt`。

## 六周学习路径

### 第 1 周：C++ 与 Qt 工程基础

阅读根 CMake、`app/main.cpp`，自己创建一个 Qt 6 Widgets 小程序。练习构建、运行、命令行参数和事件循环。

### 第 2 周：QObject、信号槽和生命周期

学习 `QObject` 父子对象树、信号槽连接方式、事件过滤器和 `deleteLater`。为当前窗口增加一个按钮、状态栏和可测试的信号槽。

### 第 3 周：Widgets 与模型视图

学习布局、`QAction`、`QSplitter`、`QTreeView`、`QListView`、`QAbstractItemModel`。做一个左侧目录树加右侧文档列表的原型。

### 第 4 周：文件、JSON 和 SQLite 边界

对照现有 `QJson*`、`QTemporaryDir` 和 SQLite 基础设施，练习错误返回、事务、UTF-8 和路径处理。不要让 UI 直接操作 SQLite，保持 facade 边界。

### 第 5 周：测试与可用性

使用 Qt Test 测试键盘操作、焦点顺序、对象名和窗口尺寸；继续使用 offscreen 模式编写可重放测试。

### 第 6 周：线程、模型刷新和性能

学习 `QThread`、worker-object 模式、跨线程信号槽和模型批量刷新。重点理解哪些工作必须离开 GUI 线程，以及取消、失败和对象销毁如何处理。

## 学习验收

完成后应能独立解释并实现：

1. 一个可测试的 `QMainWindow` 工作区；
2. 一个自定义 `QAbstractItemModel`；
3. 一个通过 signal/slot 更新 UI 的异步任务；
4. 一个使用临时目录和 offscreen 平台的 Qt Test；
5. 一个不让 UI 直接依赖 SQLite/文件实现的 application facade。

