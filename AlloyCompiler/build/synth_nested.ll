; ModuleID = 'synth_nested'
source_filename = "synth_nested"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

%alloy.struct = type { %alloy.struct.0, i32 }
%alloy.struct.0 = type { i32 }

@str = private unnamed_addr constant [19 x i8] c"inner.v=%d tag=%u\0A\00", align 1

declare ptr @malloc(i64)

declare i32 @printf(...)

define i32 @main() {
entry:
  %o = alloca %alloy.struct, align 8
  %struct.tmp1 = alloca %alloy.struct.0, align 8
  %struct.tmp = alloca %alloy.struct, align 8
  %init.f = getelementptr inbounds nuw %alloy.struct.0, ptr %struct.tmp1, i32 0, i32 0
  store i32 42, ptr %init.f, align 4
  %struct.val = load %alloy.struct.0, ptr %struct.tmp1, align 4
  %init.f2 = getelementptr inbounds nuw %alloy.struct, ptr %struct.tmp, i32 0, i32 0
  store %alloy.struct.0 %struct.val, ptr %init.f2, align 4
  %init.f3 = getelementptr inbounds nuw %alloy.struct, ptr %struct.tmp, i32 0, i32 1
  store i32 7, ptr %init.f3, align 4
  %struct.val4 = load %alloy.struct, ptr %struct.tmp, align 4
  store %alloy.struct %struct.val4, ptr %o, align 4
  %field = getelementptr inbounds nuw %alloy.struct, ptr %o, i32 0, i32 0
  %field5 = getelementptr inbounds nuw %alloy.struct.0, ptr %field, i32 0, i32 0
  %load = load i32, ptr %field5, align 4
  %field6 = getelementptr inbounds nuw %alloy.struct, ptr %o, i32 0, i32 1
  %load7 = load i32, ptr %field6, align 4
  %0 = call i32 (...) @printf(ptr @str, i32 %load, i32 %load7)
  ret i32 0
}
