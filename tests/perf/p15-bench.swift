// P15 (mac-core §2.1): the helper-line parse time per line, on the host: 1.3.0's Swift parser
// (Prepare.swift's parseRunLine, kept in the frozen core/tests/swift/oracle); the C core's parser
// gets the same lines in tests/perf/p15-bench.c. Built by tests/perf/p15.sh with the oracle's
// sources (main.swift left out).
import Foundation

let lines: [Substring] = {
    var out: [Substring] = []
    for i in 0..<2000 {
        out.append(Substring("{\"at\":\(i),\"files\":2000,\"chunk\":3,\"chunks\":21}"))
        out.append(Substring("{\"file\":\"/Users/dj/Music/Harness/track-\(i).mp3\",\"result\":\"prepare\",\"seconds\":241.5,\"entries\":22,\"gb\":0.0625,\"missing\":0}"))
    }
    out.append(Substring("{\"done\":2000,\"runs\":44000,\"run_s\":110000,\"kept\":\"7\",\"until\":1790000000,\"kept_gb\":12.5}"))
    return out
}()

var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
var perLine: [Double] = []
var sink = 0
for _ in 0..<30 {
    let s = mach_absolute_time()
    for l in lines { if parseRunLine(l) != nil { sink += 1 } }
    let ns = Double((mach_absolute_time() - s) * UInt64(tb.numer) / UInt64(tb.denom))
    perLine.append(ns / Double(lines.count))
}
perLine.sort()
print("swift \(String(format: "%.1f", perLine[perLine.count / 2]))")
print("# ns per line, median of 30 batches of \(lines.count) lines (min \(String(format: "%.1f", perLine.first!)), max \(String(format: "%.1f", perLine.last!))); parsed \(sink)")
