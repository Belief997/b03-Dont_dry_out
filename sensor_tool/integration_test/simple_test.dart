// 集成测试 —— 会真正加载 Rust 动态库, 所以这里才能验证 FRB 桥是通的。
//
// 跑法: flutter test integration_test/simple_test.dart -d windows
//
// ⚠ 不要在这里测"能不能扫到设备": 设备平时完全静默, 只有人去单击按键才播广播,
//   而且 Windows 下收包率天然很低(见 rust/src/ble/mod.rs 的文件头)。
//   那属于需要真硬件在场的手工验证, 不是自动化测试该管的事。
//   本文件只验证"桥通了、适配器能枚举"。

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:sensor_tool/src/rust/api/ble.dart';
import 'package:sensor_tool/src/rust/api/simple.dart';
import 'package:sensor_tool/src/rust/frb_generated.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async => await RustLib.init());

  test('FRB 桥可用', () {
    expect(greet(name: 'Tom'), 'Hello, Tom!');
  });

  test('适配器状态起始为 Idle', () {
    // 还没开始扫描也没连接 —— 必须是 Idle。这条能抓到"全局状态被污染"。
    expect(bleMode(), BleMode.idle);
  });

  test('能枚举蓝牙适配器(本机需开启蓝牙)', () {
    // 没有适配器时 Rust 侧抛异常 —— 那是环境问题而非代码问题, 所以这里
    // 把两种结果都算通过, 只要不是别的异常。
    //
    // ⚠ 正因为两种结果都算通过, 必须把实际走的分支打出来 —— 否则"测试通过"
    //   无法区分"真的枚举到了适配器"与"这台机器没蓝牙, 走了容错分支"。
    try {
      final name = bleAdapterName();
      // ignore: avoid_print
      print('[适配器] 枚举成功: $name');
      expect(name, isNotEmpty);
    } catch (e) {
      // ignore: avoid_print
      print('[适配器] 枚举失败(本机可能未开蓝牙): $e');
      expect('$e', contains('适配器'));
    }
  });
}
