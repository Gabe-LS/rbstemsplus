// Whether FileManager.removeItem of a folder leaves its "._" companion in the parent.
import Foundation
let parent = CommandLine.arguments[1], sub = parent + "/RB Stems Plus Cache.noindex"
try! FileManager.default.createDirectory(atPath: sub + "/ab", withIntermediateDirectories: true)
FileManager.default.createFile(atPath: sub + "/ab/x.flac", contents: Data("x".utf8))
try! FileManager.default.removeItem(atPath: sub)
let left = (try! FileManager.default.contentsOfDirectory(atPath: parent)).joined(separator: " ")
var names: [String] = []
if let d = opendir(parent) {                       // readdir: FileManager hides "._" files
    while let e = readdir(d) {
        let n = withUnsafePointer(to: &e.pointee.d_name) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        if n.hasPrefix("._") { names.append(n) }
    }
    closedir(d)
}
print("  FileManager.removeItem(folder): FileManager lists [\(left)], readdir ._ files: \(names.isEmpty ? "none" : names.joined(separator: " "))")
