import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/app/sas_word_list.dart';

void main() {
  test('matches the canonical 2048-entry pairing word list', () {
    final List<String> canonical = File(
      '../../protocol/testdata/security/v1/wordlist.txt',
    ).readAsLinesSync();

    expect(canonical, hasLength(2048));
    for (int index = 0; index < canonical.length; index += 1) {
      expect(sasWordForIndex(index), canonical[index], reason: 'index $index');
    }
    expect(() => sasWordForIndex(-1), throwsStateError);
    expect(() => sasWordForIndex(canonical.length), throwsStateError);
  });
}
