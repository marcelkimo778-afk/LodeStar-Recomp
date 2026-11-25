// Minimal stand-in for Ryujinx.Common.EmbeddedResources (Ryujinx, MIT): the shader
// translator only reads its GLSL helper snippets (GLSL back end) from the assembly.
using System.IO;
using System.Reflection;

namespace Ryujinx.Common
{
    public static class EmbeddedResources
    {
        public static string ReadAllText(string filename)
        {
            Assembly asm = typeof(EmbeddedResources).Assembly;
            string name = filename.Replace('/', '.').Replace('\\', '.');
            foreach (string res in asm.GetManifestResourceNames())
            {
                if (res.EndsWith(name))
                {
                    using Stream s = asm.GetManifestResourceStream(res);
                    using StreamReader r = new StreamReader(s);
                    return r.ReadToEnd();
                }
            }
            return string.Empty;
        }
    }
}
