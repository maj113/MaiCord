// Diagnostic-only, read-only AMD telemetry. Never linked into MaiCord.
// ABI: GPUOpen display-library/adl_structures.h and adl_defines.h (ADL 18.1).
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class AmdGpuTelemetry : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr Allocate(int bytes);
    private const string Library = "C:/Windows/System32/atiadlxx.dll";
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ADL2_Main_Control_Create(Allocate allocate, int connected, out IntPtr context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ADL2_Main_Control_Destroy(IntPtr context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ADL2_Adapter_NumberOfAdapters_Get(IntPtr context, out int count);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ADL2_Adapter_AdapterInfo_Get(IntPtr context, IntPtr adapters, int bytes);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int ADL2_New_QueryPMLogData_Get(IntPtr context, int adapter, IntPtr sensors);

    private IntPtr context, sensors;
    private readonly Allocate allocate;
    private readonly List<IntPtr> allocations = new List<IntPtr>();
    public int Adapter { get; private set; }
    public string Name { get; private set; }
    public AmdGpuTelemetry()
    {
        allocate = bytes => { var p = Marshal.AllocHGlobal(bytes); allocations.Add(p); return p; };
        try
        {
            Check(ADL2_Main_Control_Create(allocate, 1, out context));
            int count;
            Check(ADL2_Adapter_NumberOfAdapters_Get(context, out count));
            if (count < 1 || count > 64) throw new InvalidOperationException("Invalid ADL adapter count.");
            // AdapterInfo is 1572 bytes on Windows (six ANSI char[256], nine int32).
            const int stride = 1572;
            IntPtr info = Marshal.AllocHGlobal(stride * count);
            try
            {
                Marshal.Copy(new byte[stride * count], 0, info, stride * count);
                for (int i = 0; i < count; ++i) Marshal.WriteInt32(info, i * stride, stride);
                Check(ADL2_Adapter_AdapterInfo_Get(context, info, stride * count));
                Adapter = Marshal.ReadInt32(info, 4);
                Name = Marshal.PtrToStringAnsi(IntPtr.Add(info, 280), 256).TrimEnd('\0');
            }
            finally { Marshal.FreeHGlobal(info); }
            sensors = Marshal.AllocHGlobal(2052); // size + 256 {supported, value} pairs
        }
        catch { Dispose(); throw; }
    }
    private static void Check(int status)
    {
        if (status != 0) throw new InvalidOperationException("AMD telemetry status " + status);
    }
    public Dictionary<string, int?> Read()
    {
        if (context == IntPtr.Zero) throw new ObjectDisposedException("AmdGpuTelemetry");
        Marshal.Copy(new byte[2052], 0, sensors, 2052);
        Marshal.WriteInt32(sensors, 2052);
        // Deprecated by AMD for polling efficiency, but sufficient for this
        // bounded once-per-second diagnostic. No settings or clocks are changed.
        Check(ADL2_New_QueryPMLogData_Get(context, Adapter, sensors));
        var result = new Dictionary<string, int?>();
        Add(result, "GfxMHz", 1); Add(result, "MemoryMHz", 2);
        Add(result, "VcnMHz", 7); Add(result, "TemperatureC", 8);
        Add(result, "GfxPercent", 19); Add(result, "MemoryPercent", 20);
        Add(result, "AsicWatts", 23); Add(result, "BoardWatts", 73);
        return result;
    }
    private void Add(Dictionary<string, int?> result, string name, int index)
    {
        result[name] = Marshal.ReadInt32(sensors, 4 + index * 8) != 0
            ? (int?)Marshal.ReadInt32(sensors, 8 + index * 8) : null;
    }
    public void Dispose()
    {
        if (context != IntPtr.Zero) { ADL2_Main_Control_Destroy(context); context = IntPtr.Zero; }
        if (sensors != IntPtr.Zero) { Marshal.FreeHGlobal(sensors); sensors = IntPtr.Zero; }
        foreach (IntPtr p in allocations) Marshal.FreeHGlobal(p);
        allocations.Clear();
        GC.KeepAlive(allocate);
    }
}
