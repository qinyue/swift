// RUN: %target-swift-frontend %s -emit-ir -g -o - \
// RUN:    -module-name M -enable-experimental-concurrency \
// RUN:    -parse-as-library | %FileCheck %s
// REQUIRES: concurrency
// UNSUPPORTED: CPU=arm64e

public func getVegetables() async -> [String] {
  return ["leek", "carrot"]  
}

// CHECK: define {{.*}} @"$s1M14chopVegetablesSaySSGyYKF.resume.0"
public func chopVegetables() async throws -> [String] {
  let veggies = await getVegetables()
  // CHECK:  call void @llvm.dbg.declare(metadata i8* %2, metadata ![[V:[0-9]+]]
  // CHECK: ![[V]] = !DILocalVariable(name: "veggies"
  return veggies.map { "chopped \($0)" }
}
