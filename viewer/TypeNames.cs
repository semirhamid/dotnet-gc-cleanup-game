using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;

namespace ManualGC.Viewer;

/// <summary>Turns the type identities in the protocol into names, by reading assembly metadata from disk.</summary>
sealed class TypeNames
{
    abstract record Shape;
    sealed record Named(uint Module, int Token, ulong[] Args) : Shape;
    sealed record ArrayOf(int Rank, bool IsVector, ulong Element) : Shape;
    sealed record Suffixed(ulong Element, string Suffix) : Shape;
    sealed record Other(byte ElementType) : Shape;

    static readonly Dictionary<string, string> s_keywords = new()
    {
        ["System.Object"] = "object", ["System.String"] = "string", ["System.Boolean"] = "bool", ["System.Char"] = "char",
        ["System.SByte"] = "sbyte", ["System.Byte"] = "byte", ["System.Int16"] = "short", ["System.UInt16"] = "ushort",
        ["System.Int32"] = "int", ["System.UInt32"] = "uint", ["System.Int64"] = "long", ["System.UInt64"] = "ulong",
        ["System.IntPtr"] = "nint", ["System.UIntPtr"] = "nuint", ["System.Single"] = "float", ["System.Double"] = "double",
        ["System.Decimal"] = "decimal",
    };

    // Shared by every connection, and locked while in use. Each file is read only once, so the viewer
    // has to be restarted after rebuilding an app.
    static readonly Dictionary<string, MetadataReader?> s_readers = [];

    readonly Dictionary<uint, string> _modules = [];
    readonly Dictionary<ulong, Shape> _shapes = [];
    readonly Dictionary<ulong, string> _names = [];

    public void AddModule(uint id, string path) => _modules[id] = path;

    public void AddType(ProtocolReader r)
    {
        ulong id = r.U64();
        _shapes[id] = r.U8() switch
        {
            0 => new Named(r.U32(), (int)r.U32(), ReadArgs(r)),
            1 => new ArrayOf(r.U8(), r.U8() != 0, r.U64()),
            2 => new Suffixed(r.U64(), "*"),
            3 => new Suffixed(r.U64(), "&"),
            _ => new Other(r.U8()),
        };

        static ulong[] ReadArgs(ProtocolReader r)
        {
            var args = new ulong[r.U16()];
            for (int i = 0; i < args.Length; i++) args[i] = r.U64();
            return args;
        }
    }

    public string Get(ulong id)
    {
        if (!_names.TryGetValue(id, out string? name))
            _names[id] = name = Build(id);
        return name;
    }

    string Build(ulong id)
    {
        if (!_shapes.TryGetValue(id, out Shape? shape))
            return $"type 0x{id:x}";
        switch (shape)
        {
            case ArrayOf a:
                return Get(a.Element) + (a.IsVector ? "[]" : $"[{(a.Rank == 1 ? "*" : new string(',', a.Rank - 1))}]");
            case Suffixed s:
                return Get(s.Element) + s.Suffix;
            case Named n:
                string name = MetadataName(n.Module, n.Token) ?? $"type 0x{id:x}";
                if (n.Args.Length > 0)
                    name += $"<{string.Join(", ", n.Args.Select(Get))}>";
                return s_keywords.GetValueOrDefault(name, name);
            default:
                return $"type 0x{id:x}";
        }
    }

    string? MetadataName(uint module, int token)
    {
        MetadataReader? reader = _modules.TryGetValue(module, out string? path) ? Reader(path) : null;
        if (reader == null)
            return null;
        try
        {
            var handle = (TypeDefinitionHandle)MetadataTokens.EntityHandle(token);
            var parts = new List<string>();
            TypeDefinition type;
            while (true)
            {
                type = reader.GetTypeDefinition(handle);
                string part = reader.GetString(type.Name);
                int arity = part.IndexOf('`');
                parts.Insert(0, arity < 0 ? part : part[..arity]);
                if (type.GetDeclaringType().IsNil)
                    break;
                handle = type.GetDeclaringType();
            }
            var name = new StringBuilder(reader.GetString(type.Namespace));
            if (name.Length > 0)
                name.Append('.');
            return name.AppendJoin('.', parts).ToString();
        }
        catch (Exception e) when (e is BadImageFormatException or InvalidCastException or ArgumentException)
        {
            return null;
        }
    }

    static MetadataReader? Reader(string path)
    {
        lock (s_readers)
        {
            if (s_readers.TryGetValue(path, out MetadataReader? reader))
                return reader;
            try
            {
                if (path.Length > 0)
                {
                    using var file = File.OpenRead(path);
                    var pe = new PEReader(file, PEStreamOptions.PrefetchMetadata);
                    reader = pe.HasMetadata ? pe.GetMetadataReader() : null;
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or BadImageFormatException or ArgumentException)
            {
            }
            return s_readers[path] = reader;
        }
    }
}
