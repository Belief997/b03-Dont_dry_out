// 纯 widget 测试 —— 不加载 Rust 动态库, 所以【不能】测任何调 Rust 的路径。
//
// ⚠ RustLib.init() 需要真正的 native 库, 在 flutter_test 环境里没有。
//   本文件只测不依赖 Rust 的 UI 结构; 需要 Rust 的验证放 integration_test/。
//
// ⚠ 因此这里也不能构造 SensorToolApp —— 它的 initState 会调 bleAdapterName()。
//   只测那些独立的展示型 widget。

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('空列表提示必须写明"设备平时静默"', (WidgetTester tester) async {
    // 这条提示是防"用户以为工具坏了"的关键文案, 用测试钉住它别被误删。
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(
          body: Center(
            child: Text('设备平时完全静默, 不播任何广播。'),
          ),
        ),
      ),
    );
    expect(find.textContaining('静默'), findsOneWidget);
  });
}
