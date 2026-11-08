# vo-aacenc

AAC-LC encoder (VisualOn, Apache License 2.0: `COPYING`, `NOTICE`), fixed-point C. Used by
`prep/` to encode the sound of the MP4 the console prepares for the phone.

Source: https://github.com/mstorsjo/vo-aacenc, commit a277487e051e92e99a532294eed3c673f4d879f2
(2017-06-28). Only `aacenc/` (src, inc, basic_op) and `common/` are kept.

Changes:
- removed: `aacenc/src/asm` (ARMv5E / ARMv7 NEON assembly: the plain C path is used), the
  Android wrappers and makefiles, the sample code and docs;
- `aacenc/src/tns.c`: `__unused` (a bionic/BSD macro) replaced by `__attribute__((unused))`.
