; ModuleID = 'C:\Users\Maj\AppData\Local\Temp/mcm2/sc.cpp'
source_filename = "C:\\Users\\Maj\\AppData\\Local\\Temp/mcm2/sc.cpp"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc19.51.36246"

%struct.R = type { ptr, i32, i64, i32, i32, i32 }

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind sspstrong memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define dso_local void @"?refill@@YAXPEAUR@@@Z"(ptr nofree noundef captures(none) %0) local_unnamed_addr #0 {
  %2 = load ptr, ptr %0, align 8
  %3 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %4 = load i32, ptr %3, align 8
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 28
  %6 = load i32, ptr %5, align 4
  %7 = icmp ult i32 %6, 57
  br i1 %7, label %8, label %144

8:                                                ; preds = %1
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %10 = load i32, ptr %9, align 8
  %11 = icmp eq i32 %10, 0
  %12 = getelementptr inbounds nuw i8, ptr %0, i64 24
  %13 = getelementptr inbounds nuw i8, ptr %0, i64 16
  br i1 %11, label %14, label %144

14:                                               ; preds = %8
  %15 = load i32, ptr %12, align 8
  %16 = zext i32 %15 to i64
  %17 = zext nneg i32 %6 to i64
  %18 = tail call i32 @llvm.umax.i32(i32 %4, i32 %15)
  %19 = zext i32 %18 to i64
  %20 = icmp ult i32 %15, %4
  br i1 %20, label %21, label %142

21:                                               ; preds = %14
  %22 = getelementptr inbounds nuw i8, ptr %2, i64 %16
  %23 = load i8, ptr %22, align 1
  %24 = icmp eq i8 %23, -1
  br i1 %24, label %142, label %25

25:                                               ; preds = %21
  %26 = add nuw nsw i64 %16, 1
  %27 = trunc nuw i64 %26 to i32
  store i32 %27, ptr %12, align 8
  %28 = zext i8 %23 to i64
  %29 = sub nuw nsw i64 56, %17
  %30 = shl nuw i64 %28, %29
  %31 = load i64, ptr %13, align 8
  %32 = or i64 %31, %30
  store i64 %32, ptr %13, align 8
  %33 = add nuw nsw i32 %6, 8
  store i32 %33, ptr %5, align 4
  %34 = icmp ult i32 %6, 49
  br i1 %34, label %35, label %144

35:                                               ; preds = %25
  %36 = icmp eq i64 %26, %19
  br i1 %36, label %142, label %37

37:                                               ; preds = %35
  %38 = getelementptr inbounds nuw i8, ptr %2, i64 %26
  %39 = load i8, ptr %38, align 1
  %40 = icmp eq i8 %39, -1
  br i1 %40, label %142, label %41

41:                                               ; preds = %37
  %42 = add nuw nsw i64 %16, 2
  %43 = trunc nuw i64 %42 to i32
  store i32 %43, ptr %12, align 8
  %44 = zext i8 %39 to i64
  %45 = sub nuw nsw i64 48, %17
  %46 = shl nuw i64 %44, %45
  %47 = load i64, ptr %13, align 8
  %48 = or i64 %47, %46
  store i64 %48, ptr %13, align 8
  %49 = add nuw nsw i32 %6, 16
  store i32 %49, ptr %5, align 4
  %50 = icmp ult i32 %6, 41
  br i1 %50, label %51, label %144

51:                                               ; preds = %41
  %52 = icmp eq i64 %42, %19
  br i1 %52, label %142, label %53

53:                                               ; preds = %51
  %54 = getelementptr inbounds nuw i8, ptr %2, i64 %42
  %55 = load i8, ptr %54, align 1
  %56 = icmp eq i8 %55, -1
  br i1 %56, label %142, label %57

57:                                               ; preds = %53
  %58 = add nuw nsw i64 %16, 3
  %59 = trunc nuw i64 %58 to i32
  store i32 %59, ptr %12, align 8
  %60 = zext i8 %55 to i64
  %61 = sub nuw nsw i64 40, %17
  %62 = shl nuw i64 %60, %61
  %63 = load i64, ptr %13, align 8
  %64 = or i64 %63, %62
  store i64 %64, ptr %13, align 8
  %65 = add nuw nsw i32 %6, 24
  store i32 %65, ptr %5, align 4
  %66 = icmp ult i32 %6, 33
  br i1 %66, label %67, label %144

67:                                               ; preds = %57
  %68 = icmp eq i64 %58, %19
  br i1 %68, label %142, label %69

69:                                               ; preds = %67
  %70 = getelementptr inbounds nuw i8, ptr %2, i64 %58
  %71 = load i8, ptr %70, align 1
  %72 = icmp eq i8 %71, -1
  br i1 %72, label %142, label %73

73:                                               ; preds = %69
  %74 = add nuw nsw i64 %16, 4
  %75 = trunc nuw i64 %74 to i32
  store i32 %75, ptr %12, align 8
  %76 = zext i8 %71 to i64
  %77 = sub nuw nsw i64 32, %17
  %78 = shl nuw i64 %76, %77
  %79 = load i64, ptr %13, align 8
  %80 = or i64 %79, %78
  store i64 %80, ptr %13, align 8
  %81 = add nuw nsw i32 %6, 32
  store i32 %81, ptr %5, align 4
  %82 = icmp ult i32 %6, 25
  br i1 %82, label %83, label %144

83:                                               ; preds = %73
  %84 = icmp eq i64 %74, %19
  br i1 %84, label %142, label %85

85:                                               ; preds = %83
  %86 = getelementptr inbounds nuw i8, ptr %2, i64 %74
  %87 = load i8, ptr %86, align 1
  %88 = icmp eq i8 %87, -1
  br i1 %88, label %142, label %89

89:                                               ; preds = %85
  %90 = add nuw nsw i64 %16, 5
  %91 = trunc nuw i64 %90 to i32
  store i32 %91, ptr %12, align 8
  %92 = zext i8 %87 to i64
  %93 = sub nuw nsw i64 24, %17
  %94 = shl nuw nsw i64 %92, %93
  %95 = load i64, ptr %13, align 8
  %96 = or i64 %95, %94
  store i64 %96, ptr %13, align 8
  %97 = add nuw nsw i32 %6, 40
  store i32 %97, ptr %5, align 4
  %98 = icmp ult i32 %6, 17
  br i1 %98, label %99, label %144

99:                                               ; preds = %89
  %100 = icmp eq i64 %90, %19
  br i1 %100, label %142, label %101

101:                                              ; preds = %99
  %102 = getelementptr inbounds nuw i8, ptr %2, i64 %90
  %103 = load i8, ptr %102, align 1
  %104 = icmp eq i8 %103, -1
  br i1 %104, label %142, label %105

105:                                              ; preds = %101
  %106 = add nuw nsw i64 %16, 6
  %107 = trunc nuw i64 %106 to i32
  store i32 %107, ptr %12, align 8
  %108 = zext i8 %103 to i64
  %109 = sub nuw nsw i64 16, %17
  %110 = shl nuw nsw i64 %108, %109
  %111 = load i64, ptr %13, align 8
  %112 = or i64 %111, %110
  store i64 %112, ptr %13, align 8
  %113 = add nuw nsw i32 %6, 48
  store i32 %113, ptr %5, align 4
  %114 = icmp ult i32 %6, 9
  br i1 %114, label %115, label %144

115:                                              ; preds = %105
  %116 = icmp eq i64 %106, %19
  br i1 %116, label %142, label %117

117:                                              ; preds = %115
  %118 = getelementptr inbounds nuw i8, ptr %2, i64 %106
  %119 = load i8, ptr %118, align 1
  %120 = icmp eq i8 %119, -1
  br i1 %120, label %142, label %121

121:                                              ; preds = %117
  %122 = add nuw nsw i64 %16, 7
  %123 = trunc nuw i64 %122 to i32
  store i32 %123, ptr %12, align 8
  %124 = zext i8 %119 to i64
  %125 = sub nuw nsw i64 8, %17
  %126 = shl nuw nsw i64 %124, %125
  %127 = load i64, ptr %13, align 8
  %128 = or i64 %127, %126
  store i64 %128, ptr %13, align 8
  %129 = add nuw nsw i32 %6, 56
  store i32 %129, ptr %5, align 4
  %130 = icmp eq i32 %6, 0
  br i1 %130, label %131, label %144

131:                                              ; preds = %121
  %132 = icmp eq i64 %122, %19
  br i1 %132, label %142, label %133

133:                                              ; preds = %131
  %134 = getelementptr inbounds nuw i8, ptr %2, i64 %122
  %135 = load i8, ptr %134, align 1
  %136 = icmp eq i8 %135, -1
  br i1 %136, label %142, label %137

137:                                              ; preds = %133
  %138 = add i32 %15, 8
  store i32 %138, ptr %12, align 8
  %139 = zext i8 %135 to i64
  %140 = load i64, ptr %13, align 8
  %141 = or i64 %140, %139
  store i64 %141, ptr %13, align 8
  store i32 64, ptr %5, align 4
  br label %144

142:                                              ; preds = %133, %131, %117, %115, %101, %99, %85, %83, %69, %67, %53, %51, %37, %35, %21, %14
  %143 = phi i32 [ 1, %14 ], [ 2, %21 ], [ 1, %35 ], [ 2, %37 ], [ 1, %51 ], [ 2, %53 ], [ 1, %67 ], [ 2, %69 ], [ 1, %83 ], [ 2, %85 ], [ 1, %99 ], [ 2, %101 ], [ 1, %115 ], [ 2, %117 ], [ 1, %131 ], [ 2, %133 ]
  store i32 %143, ptr %9, align 8
  br label %144

144:                                              ; preds = %25, %41, %57, %73, %89, %105, %121, %137, %142, %1, %8
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #1

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind sspstrong memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define dso_local noundef i32 @"?f@@YAIPEAUR@@@Z"(ptr nofree noundef captures(none) %0) local_unnamed_addr #0 {
  %2 = alloca %struct.R, align 8
  call void @llvm.lifetime.start.p0(ptr nonnull %2) #3
  call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 8 dereferenceable(40) %2, ptr noundef nonnull align 8 dereferenceable(40) %0, i64 40, i1 false)
  %3 = getelementptr inbounds nuw i8, ptr %2, i64 28
  %4 = load i32, ptr %3, align 4
  %5 = icmp eq i32 %4, 0
  br i1 %5, label %6, label %8

6:                                                ; preds = %1
  call void @"?refill@@YAXPEAUR@@@Z"(ptr noundef nonnull %2)
  %7 = load i32, ptr %3, align 4
  br label %8

8:                                                ; preds = %6, %1
  %9 = phi i32 [ %7, %6 ], [ %4, %1 ]
  call void @llvm.memcpy.p0.p0.i64(ptr noundef nonnull align 8 dereferenceable(40) %0, ptr noundef nonnull align 8 dereferenceable(40) %2, i64 40, i1 false)
  call void @llvm.lifetime.end.p0(ptr nonnull %2) #3
  ret i32 %9
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #1

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.umax.i32(i32, i32) #2

attributes #0 = { mustprogress nofree noinline norecurse nosync nounwind sspstrong memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { nounwind }

!llvm.dbg.cu = !{!0}
!llvm.linker.options = !{!2, !3}
!llvm.module.flags = !{!4, !5, !6, !7}
!llvm.ident = !{!8}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "clang version 24.0.0git (https://github.com/maj113/llvm-pe 90effcbeb3f6ac2ddd7d55d7a190ff5968b6575a)", isOptimized: true, runtimeVersion: 0, emissionKind: NoDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "C:\\Users\\Maj\\AppData\\Local\\Temp/mcm2\\sc.cpp", directory: "D:\\MaiCord")
!2 = !{!"/DEFAULTLIB:libcmt.lib"}
!3 = !{!"/DEFAULTLIB:oldnames.lib"}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = !{i32 8, !"PIC Level", i32 2}
!6 = !{i32 7, !"uwtable", i32 2}
!7 = !{i32 1, !"MaxTLSAlign", i32 65536}
!8 = !{!"clang version 24.0.0git (https://github.com/maj113/llvm-pe 90effcbeb3f6ac2ddd7d55d7a190ff5968b6575a)"}
