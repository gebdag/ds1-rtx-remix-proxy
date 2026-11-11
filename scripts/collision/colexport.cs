// Exports every collision mesh of a DS1 map's hkxbhd/hkxbdt as an OBJ (one per collision model), via SoulsFormats.
// Usage: colexport <map folder> <out folder>
using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using SoulsFormats;

string mapDir = args[0], outDir = args[1];
Directory.CreateDirectory(outDir);
string id = Path.GetFileName(mapDir.TrimEnd('\\', '/'));
var bnd = BXF3.Read(new Memory<byte>(File.ReadAllBytes(Path.Combine(mapDir, "h" + id.Substring(1) + ".hkxbhd"))), new Memory<byte>(File.ReadAllBytes(Path.Combine(mapDir, "h" + id.Substring(1) + ".hkxbdt"))));

static object FirstField(object o) => o.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance)[0].GetValue(o);

foreach (var f in bnd.Files)
{
    var bytes = f.Bytes;
    if (DCX.Is(bytes)) bytes = DCX.Decompress(bytes);
    HKX hkx;
    try { hkx = HKX.Read(bytes, HKX.HKXVariation.HKXDS1, true); }
    catch (Exception e) { Console.WriteLine($"{f.Name}: read failed {e.Message}"); continue; }
    var sb = new StringBuilder();
    int vbase = 1, verts = 0, tris = 0, parts = 0;
    foreach (var o in hkx.DataSection.Objects)
    {
        if (o is not HKX.HKPStorageExtendedMeshShapeMeshSubpartStorage s) continue;
        parts++;
        var vs = s.Vertices.GetArrayData().Elements;
        foreach (var v in vs) sb.AppendLine($"v {v.Vector.X} {v.Vector.Y} {v.Vector.Z}");
        var i16 = s.Indices16?.GetArrayData()?.Elements;
        var i32 = s.Indices32?.GetArrayData()?.Elements;
        var i8 = s.Indices8?.GetArrayData()?.Elements;
        var list = new System.Collections.Generic.List<long>();
        if (i16 != null && i16.Count > 0) foreach (var x in i16) list.Add(Convert.ToInt64(FirstField(x)));
        else if (i32 != null && i32.Count > 0) foreach (var x in i32) list.Add(Convert.ToInt64(FirstField(x)));
        else if (i8 != null && i8.Count > 0) foreach (var x in i8) list.Add(Convert.ToInt64(FirstField(x)));
        // hkpExtendedMeshShape triangles are stored 4 indices apart (the fourth is padding) in FromSoftware's collisions.
        int stride = list.Count % 4 == 0 && list.Where((_, k) => k % 4 == 3).All(x => x == 0) ? 4 : 3;
        for (int k = 0; k + 2 < list.Count; k += stride)
        {
            sb.AppendLine($"f {list[k] + vbase} {list[k + 1] + vbase} {list[k + 2] + vbase}");
            tris++;
        }
        vbase += vs.Count;
        verts += vs.Count;
    }
    string name = Path.GetFileNameWithoutExtension(f.Name.Replace(".dcx", ""));
    File.WriteAllText(Path.Combine(outDir, name + ".obj"), sb.ToString());
    Console.WriteLine($"{name}: {parts} parts, {verts} vertices, {tris} triangles");
}
