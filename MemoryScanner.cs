using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

class MemoryScanner
{
    // Win32 API declarations
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint processAccess, bool bInheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern int VirtualQueryEx(IntPtr hProcess, IntPtr lpAddress, out MEMORY_BASIC_INFORMATION lpBuffer, uint dwLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, [Out] byte[] lpBuffer, int dwSize, out IntPtr lpNumberOfBytesRead);

    // Process permissions
    public const uint PROCESS_VM_READ = 0x0010;
    public const uint PROCESS_QUERY_INFORMATION = 0x0400;

    // Correct MEMORY_BASIC_INFORMATION for x64 with explicit padding
    [StructLayout(LayoutKind.Sequential)]
    public struct MEMORY_BASIC_INFORMATION
    {
        public ulong BaseAddress;         // PVOID  - 8 bytes
        public ulong AllocationBase;      // PVOID  - 8 bytes
        public uint AllocationProtect;    // DWORD  - 4 bytes
        public uint __alignment1;         // padding - 4 bytes (align to 8)
        public ulong RegionSize;          // SIZE_T - 8 bytes
        public uint State;                // DWORD  - 4 bytes
        public uint Protect;              // DWORD  - 4 bytes
        public uint Type;                 // DWORD  - 4 bytes
        public uint __alignment2;         // padding - 4 bytes (struct align to 8)
    }

    // Constants for memory states
    public const uint MEM_COMMIT  = 0x1000;
    public const uint MEM_FREE    = 0x10000;
    public const uint MEM_RESERVE = 0x2000;

    // Memory protection flags
    public const uint PAGE_NOACCESS  = 0x01;
    public const uint PAGE_GUARD     = 0x100;

    // 4 KB page size
    public const int PAGE_SIZE = 4096;

    class PageLocation
    {
        public string ProcessName { get; set; }
        public int ProcessId { get; set; }
        public long Address { get; set; }
    }

    static long totalBytesScanned = 0;
    static long totalZeroPages = 0;
    static long totalReadFailures = 0;
    static long totalUniquePages = 0;
    static Dictionary<string, List<PageLocation>> pageRegistry = new Dictionary<string, List<PageLocation>>();

    static void Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        Console.Title = "RAM Duplicate Page Scanner";

        Console.Clear();
        Console.ForegroundColor = ConsoleColor.Cyan;
        Console.WriteLine("=================================================================");
        Console.WriteLine("     Scanner duplikatov operativnoj pamyati (RAM)                ");
        Console.WriteLine("=================================================================");
        Console.ResetColor();

        Console.WriteLine("Process mode: {0}", Environment.Is64BitProcess ? "64-bit" : "32-bit");
        Console.WriteLine("MEMORY_BASIC_INFORMATION size: {0} bytes", Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)));

        // 1. Show running processes and memory usage
        Console.WriteLine("\nGetting process list...");
        Process[] processes = Process.GetProcesses();
        List<Process> targetProcesses = new List<Process>();

        List<Process> activeProcs = new List<Process>();
        foreach (var p in processes)
        {
            try
            {
                if (p.Id == 0 || p.Id == 4) continue;
                if (p.PrivateMemorySize64 > 0)
                {
                    activeProcs.Add(p);
                }
            }
            catch { }
        }

        activeProcs.Sort((a, b) => b.PrivateMemorySize64.CompareTo(a.PrivateMemorySize64));

        Console.WriteLine("\nTop-15 processes by memory usage:");
        Console.WriteLine("{0,-10} {1,-30} {2,15}", "PID", "Process Name", "Memory (MB)");
        Console.WriteLine(new string('-', 60));
        for (int i = 0; i < Math.Min(15, activeProcs.Count); i++)
        {
            var p = activeProcs[i];
            Console.WriteLine("{0,-10} {1,-30} {2,15:N1}", p.Id, p.ProcessName, p.PrivateMemorySize64 / 1024.0 / 1024.0);
        }

        Console.ForegroundColor = ConsoleColor.Yellow;
        Console.Write("\nEnter process name (e.g. chrome, node, powershell) or PID: ");
        Console.ResetColor();
        string rawInput = Console.ReadLine();
        string input = rawInput != null ? rawInput.Trim() : null;

        if (string.IsNullOrEmpty(input))
        {
            Console.WriteLine("Invalid input. Exiting.");
            return;
        }

        int targetPid = -1;
        bool isPid = int.TryParse(input, out targetPid);

        foreach (var p in activeProcs)
        {
            if (isPid)
            {
                if (p.Id == targetPid) targetProcesses.Add(p);
            }
            else
            {
                if (p.ProcessName.Equals(input, StringComparison.OrdinalIgnoreCase))
                {
                    targetProcesses.Add(p);
                }
            }
        }

        if (targetProcesses.Count == 0)
        {
            Console.ForegroundColor = ConsoleColor.Red;
            Console.WriteLine("Process not found.");
            Console.ResetColor();
            return;
        }

        Console.ForegroundColor = ConsoleColor.Green;
        Console.WriteLine("Found {0} process(es) to scan:", targetProcesses.Count);
        foreach (var p in targetProcesses)
        {
            Console.WriteLine("  - {0} (PID: {1}) = {2:N1} MB", p.ProcessName, p.Id, p.PrivateMemorySize64 / 1024.0 / 1024.0);
        }
        Console.ResetColor();

        Console.WriteLine("\nPress any key to start scanning...");
        Console.ReadKey(true);

        Console.Clear();
        Console.WriteLine("Starting scan...\n");

        using (MD5 md5 = MD5.Create())
        {
            byte[] pageBuffer = new byte[PAGE_SIZE];

            foreach (var proc in targetProcesses)
            {
                IntPtr hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, proc.Id);
                if (hProcess == IntPtr.Zero)
                {
                    int err = Marshal.GetLastWin32Error();
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine("[FAIL] Cannot open {0} (PID: {1}). Error code: {2}. Run as Administrator!", proc.ProcessName, proc.Id, err);
                    Console.ResetColor();
                    continue;
                }

                Console.WriteLine("Scanning {0} (PID: {1})...", proc.ProcessName, proc.Id);

                ulong address = 0;
                MEMORY_BASIC_INFORMATION mbi;
                uint structSize = (uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION));

                int regionsTotal = 0;
                int regionsCommitted = 0;
                int regionsScanned = 0;
                int debugPrinted = 0;

                while (true)
                {
                    int result = VirtualQueryEx(hProcess, (IntPtr)address, out mbi, structSize);
                    if (result == 0) break;

                    regionsTotal++;

                    // Debug: print first 5 regions
                    if (debugPrinted < 5)
                    {
                        Console.WriteLine("  [DEBUG] Region #{0}: Base=0x{1:X}, Size={2} KB, State=0x{3:X}, Protect=0x{4:X}, Type=0x{5:X}",
                            regionsTotal, mbi.BaseAddress, mbi.RegionSize / 1024, mbi.State, mbi.Protect, mbi.Type);
                        debugPrinted++;
                    }

                    if (mbi.State == MEM_COMMIT)
                    {
                        regionsCommitted++;
                        bool isGuard = (mbi.Protect & PAGE_GUARD) != 0;
                        bool isNoAccess = (mbi.Protect & PAGE_NOACCESS) != 0;

                        if (!isGuard && !isNoAccess && mbi.Protect != 0)
                        {
                            regionsScanned++;
                            ulong start = mbi.BaseAddress;
                            ulong size = mbi.RegionSize;
                            ulong end = start + size;

                            for (ulong curr = start; curr < end; curr += PAGE_SIZE)
                            {
                                IntPtr bytesRead;
                                if (ReadProcessMemory(hProcess, (IntPtr)curr, pageBuffer, PAGE_SIZE, out bytesRead) && (long)bytesRead == PAGE_SIZE)
                                {
                                    totalBytesScanned += PAGE_SIZE;

                                    if (IsAllZeros(pageBuffer))
                                    {
                                        totalZeroPages++;
                                    }
                                    else
                                    {
                                        byte[] hashBytes = md5.ComputeHash(pageBuffer);
                                        string hashStr = BitConverter.ToString(hashBytes).Replace("-", "");

                                        var loc = new PageLocation
                                        {
                                            ProcessName = proc.ProcessName,
                                            ProcessId = proc.Id,
                                            Address = (long)curr
                                        };

                                        List<PageLocation> list;
                                        if (pageRegistry.TryGetValue(hashStr, out list))
                                        {
                                            list.Add(loc);
                                        }
                                        else
                                        {
                                            var newList = new List<PageLocation> { loc };
                                            pageRegistry[hashStr] = newList;
                                            totalUniquePages++;
                                        }
                                    }
                                }
                                else
                                {
                                    totalReadFailures++;
                                }
                            }
                        }
                    }

                    ulong nextAddr = mbi.BaseAddress + mbi.RegionSize;
                    if (nextAddr <= address) break; // overflow protection
                    address = nextAddr;
                }

                Console.WriteLine("  Regions total: {0}, committed: {1}, scanned (readable): {2}", regionsTotal, regionsCommitted, regionsScanned);
                CloseHandle(hProcess);
            }
        }

        PrintReport();
    }

    static bool IsAllZeros(byte[] buffer)
    {
        for (int i = 0; i < buffer.Length; i++)
        {
            if (buffer[i] != 0) return false;
        }
        return true;
    }

    static void PrintReport()
    {
        Console.ForegroundColor = ConsoleColor.Cyan;
        Console.WriteLine("\n=================================================================");
        Console.WriteLine("                        SCAN REPORT                              ");
        Console.WriteLine("=================================================================");
        Console.ResetColor();

        double scannedMb = totalBytesScanned / 1024.0 / 1024.0;
        double zeroMb = (totalZeroPages * PAGE_SIZE) / 1024.0 / 1024.0;

        Console.WriteLine("Total memory scanned:        {0:N2} MB ({1:N0} pages)", scannedMb, totalBytesScanned / PAGE_SIZE);
        Console.WriteLine("Zero-filled pages:           {0:N2} MB ({1:N0} pages)", zeroMb, totalZeroPages);
        Console.WriteLine("Read failures:               {0:N0} pages", totalReadFailures);

        long duplicatePagesCount = 0;
        long duplicateBytes = 0;
        int duplicateGroups = 0;

        List<KeyValuePair<string, List<PageLocation>>> sortedDupes = new List<KeyValuePair<string, List<PageLocation>>>();

        foreach (var entry in pageRegistry)
        {
            if (entry.Value.Count > 1)
            {
                duplicateGroups++;
                int dupesCount = entry.Value.Count - 1;
                duplicatePagesCount += dupesCount;
                duplicateBytes += dupesCount * PAGE_SIZE;

                sortedDupes.Add(entry);
            }
        }

        sortedDupes.Sort((a, b) => b.Value.Count.CompareTo(a.Value.Count));

        double dupeMb = duplicateBytes / 1024.0 / 1024.0;
        double totalPotentialSavingsMb = dupeMb + zeroMb;

        Console.ForegroundColor = ConsoleColor.Green;
        Console.WriteLine("Unique non-zero pages:       {0:N0}", totalUniquePages);
        Console.WriteLine("Duplicate groups:            {0:N0}", duplicateGroups);
        Console.WriteLine("Redundant page copies:       {0:N0} pages ({1:N2} MB)", duplicatePagesCount, dupeMb);
        Console.WriteLine("\nPOTENTIAL MEMORY SAVINGS:");
        Console.WriteLine("  Deduplication savings:     {0:N2} MB", dupeMb);
        Console.WriteLine("  Zero-page savings:         {0:N2} MB", zeroMb);
        
        double percentage = scannedMb > 0 ? (totalPotentialSavingsMb / scannedMb * 100) : 0;
        Console.WriteLine("  TOTAL saveable:            {0:N2} MB ({1:N1}% of scanned memory)", totalPotentialSavingsMb, percentage);
        Console.ResetColor();

        if (sortedDupes.Count > 0)
        {
            Console.WriteLine("\nTop-10 most duplicated memory blocks:");
            Console.WriteLine("{0,-6} {1,-8} {2,-20} {3}", "#", "Copies", "First Address", "Processes");
            Console.WriteLine(new string('-', 80));
            for (int i = 0; i < Math.Min(10, sortedDupes.Count); i++)
            {
                var dupe = sortedDupes[i];
                var sample = dupe.Value[0];
                
                HashSet<string> procNames = new HashSet<string>();
                foreach (var loc in dupe.Value)
                {
                    procNames.Add(string.Format("{0}({1})", loc.ProcessName, loc.ProcessId));
                }
                string procsString = string.Join(", ", procNames);
                if (procsString.Length > 40) procsString = procsString.Substring(0, 37) + "...";

                Console.WriteLine("{0,-6} {1,-8} 0x{2:X12}   {3}", i + 1, dupe.Value.Count, sample.Address, procsString);
            }
        }
        else
        {
            Console.WriteLine("\n[INFO] No duplicate non-zero pages found.");
        }

        Console.WriteLine("\nPress any key to exit...");
        Console.ReadKey(true);
    }
}
