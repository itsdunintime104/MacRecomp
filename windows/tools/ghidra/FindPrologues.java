// Creates functions at MSVC x86 prologues found inside coverage gaps of the real code region.
// Usage: -postScript FindPrologues.java <rounds> <tier>
//   tier "prologue": candidates are gap addresses that start a known prologue pattern
//   (strong: 55 8B EC / 8B FF 55 8B EC; weak: push reg, sub esp: only with call or
//   pointer evidence). Every candidate must disassemble cleanly to a terminator.
//   tier "evidence": also accepts any gap address that is a direct-call target or a pointer
//   target (.rdata/.data/.text pointer tables), with any first byte, if it disassembles cleanly.
//   tier "padded": evidence, plus any gap address that follows 2+ 0xCC bytes (inter-function
//   padding) and disassembles cleanly to a terminator (FPO functions with no pattern).
//   tier "adjacent": padded, plus gap starts (the byte after an existing function) and the
//   byte after a ret (C3, C2 imm16) inside a gap, when the next byte is not 0xCC.
// Before the scan, 4-aligned runs of 4+ pointers into the code region (exception funclet
// tables, vtables) found in gaps are defined as pointer data and their targets count as
// evidence.
// @category Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class FindPrologues extends GhidraScript {

    private static final long REGION_START = 0x401000L;
    private static final long REGION_END = 0xac3786L; // inclusive
    private static final int MAX_INSNS = 20000;
    private static final Set<String> BAD = Set.of(
            "IN", "OUT", "INS", "OUTS", "HLT", "CLI", "STI", "ARPL", "LDS", "LES",
            "INTO", "INT", "IRET", "IRETD", "LGDT", "LIDT", "LLDT", "LTR", "CLTS", "SYSEXIT",
            "SYSRET", "AAA", "AAS", "AAM", "AAD", "DAA", "DAS",
            "BOUND", "XLAT", "LOOP", "LOOPE", "LOOPNE", "JECXZ", "JCXZ", "ENTER", "PUSHA",
            "POPA", "PUSHAD", "POPAD", "RETF", "CALLF", "JMPF", "UD2");

    private int debugLeft = 0;
    private int debugRej = 40;
    private String lastReason = "";
    private final java.util.Map<String, Integer> reasons = new java.util.TreeMap<>();

    private AddressSet fail(String why) {
        lastReason = why;
        reasons.merge(why, 1, Integer::sum);
        return null;
    }

    private Address addr(long off) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(off);
    }

    // Instructions in the region that belong to no function are leftovers of speculative
    // disassembly (for example the aggressive instruction finder). They block clean
    // disassembly of real entries, so remove them before the scan.
    private int clearOrphanInstructions() {
        AddressSet nonFunc = new AddressSet(addr(REGION_START), addr(REGION_END));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            nonFunc.delete(f.getBody());
        }
        AddressSet orphan = new AddressSet();
        int n = 0;
        for (Instruction i : currentProgram.getListing().getInstructions(nonFunc, true)) {
            orphan.add(i.getMinAddress(), i.getMaxAddress());
            n++;
        }
        if (n > 0) {
            for (AddressRange r : orphan) {
                currentProgram.getListing().clearCodeUnits(r.getMinAddress(), r.getMaxAddress(), false);
            }
        }
        return n;
    }

    private AddressSet computeGaps() {
        AddressSet gaps = new AddressSet(addr(REGION_START), addr(REGION_END));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            gaps.delete(f.getBody());
        }
        for (Data d : currentProgram.getListing().getDefinedData(gaps, true)) {
            gaps.delete(d.getMinAddress(), d.getMaxAddress());
        }
        // Instructions that belong to no function stay out of the scan.
        AddressSet orphan = new AddressSet();
        for (Instruction i : currentProgram.getListing().getInstructions(gaps, true)) {
            orphan.add(i.getMinAddress(), i.getMaxAddress());
        }
        gaps.delete(orphan);
        return gaps;
    }

    private static boolean inRegion(byte[] buf, int i) {
        if (i + 4 > buf.length) {
            return false;
        }
        long v = (buf[i] & 0xffL) | ((buf[i + 1] & 0xffL) << 8)
                | ((buf[i + 2] & 0xffL) << 16) | ((buf[i + 3] & 0xffL) << 24);
        return v >= REGION_START && v <= REGION_END;
    }

    // Defines runs of 4+ consecutive in-region pointers (any alignment) in gaps as pointer data.
    private int definePointerRuns(AddressSetView gaps) throws Exception {
        int runs = 0;
        for (AddressRange r : gaps) {
            if (r.getLength() < 16) {
                continue;
            }
            long lo = r.getMinAddress().getOffset();
            byte[] buf = new byte[(int) r.getLength()];
            currentProgram.getMemory().getBytes(r.getMinAddress(), buf);
            int i = 0;
            while (i + 4 <= buf.length) {
                if (!inRegion(buf, i)) {
                    i++;
                    continue;
                }
                int j = i;
                while (inRegion(buf, j)) {
                    j += 4;
                }
                if ((j - i) / 4 >= 4) {
                    for (int k = i; k < j; k += 4) {
                        createData(addr(lo + k), ghidra.program.model.data.PointerDataType.dataType);
                    }
                    runs++;
                    i = j;
                } else {
                    i++;
                }
            }
        }
        return runs;
    }

    private Set<Long> collectPointerTargets(AddressSetView gaps) throws Exception {
        Set<Long> out = new HashSet<>();
        for (Data d : currentProgram.getListing().getDefinedData(true)) {
            if (d.getAddress().getOffset() >= REGION_START && d.getAddress().getOffset() <= REGION_END
                    && d.isPointer() && d.getValue() instanceof Address) {
                Address t = (Address) d.getValue();
                if (gaps.contains(t)) {
                    out.add(t.getOffset());
                }
            }
        }
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isInitialized() || b.getName().equals(".text")) {
                continue;
            }
            byte[] buf = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), buf);
            for (int i = 0; i + 4 <= buf.length; i += 4) {
                long v = (buf[i] & 0xffL) | ((buf[i + 1] & 0xffL) << 8)
                        | ((buf[i + 2] & 0xffL) << 16) | ((buf[i + 3] & 0xffL) << 24);
                if (v >= REGION_START && v <= REGION_END && gaps.contains(addr(v))) {
                    out.add(v);
                }
            }
        }
        // Defined pointers inside .text are mostly switch tables to mid-function labels, skipped.
        return out;
    }

    private Set<Long> collectCallTargets(AddressSetView gaps) throws Exception {
        Set<Long> out = new HashSet<>();
        // Direct calls from existing instructions.
        for (Instruction i : currentProgram.getListing().getInstructions(true)) {
            if (!i.getFlowType().isCall()) {
                continue;
            }
            for (Address t : i.getFlows()) {
                if (gaps.contains(t)) {
                    out.add(t.getOffset());
                }
            }
        }
        // Raw E8 rel32 inside the gaps (code in a gap calling another gap function).
        for (AddressRange r : gaps) {
            long lo = r.getMinAddress().getOffset();
            long n = r.getLength();
            if (n < 5) {
                continue;
            }
            byte[] buf = new byte[(int) n];
            currentProgram.getMemory().getBytes(r.getMinAddress(), buf);
            for (int i = 0; i + 5 <= buf.length; i++) {
                if ((buf[i] & 0xff) != 0xE8) {
                    continue;
                }
                int rel = (buf[i + 1] & 0xff) | ((buf[i + 2] & 0xff) << 8)
                        | ((buf[i + 3] & 0xff) << 16) | ((buf[i + 4] & 0xff) << 24);
                long t = (lo + i + 5 + rel) & 0xffffffffL;
                if (t >= REGION_START && t <= REGION_END && gaps.contains(addr(t))) {
                    out.add(t);
                }
            }
        }
        return out;
    }

    // 2 = strong prologue, 1 = weak (needs call/pointer evidence), 0 = none.
    private int prologueStrength(byte[] b, int i) {
        int p = i;
        if (p + 5 <= b.length && (b[p] & 0xff) == 0x8B && (b[p + 1] & 0xff) == 0xFF
                && (b[p + 2] & 0xff) == 0x55 && (b[p + 3] & 0xff) == 0x8B && (b[p + 4] & 0xff) == 0xEC) {
            return 2;
        }
        if (p + 3 <= b.length && (b[p] & 0xff) == 0x55 && (b[p + 1] & 0xff) == 0x8B
                && (b[p + 2] & 0xff) == 0xEC) {
            return 2;
        }
        int q = p;
        while (q < b.length && q - p < 3
                && ((b[q] & 0xff) == 0x53 || (b[q] & 0xff) == 0x56 || (b[q] & 0xff) == 0x57
                    || (b[q] & 0xff) == 0x55)) {
            q++;
        }
        if (q > p) {
            return 1;
        }
        if (p + 3 <= b.length && (b[p] & 0xff) == 0x83 && (b[p + 1] & 0xff) == 0xEC) {
            return 1;
        }
        if (p + 6 <= b.length && (b[p] & 0xff) == 0x81 && (b[p + 1] & 0xff) == 0xEC) {
            return 1;
        }
        return 0;
    }

    // Walks the code reachable from entry inside the gap set. Returns the instruction
    // addresses on success or null when the flow is not clean.
    private AddressSet validate(Address entry, AddressSetView gaps) throws Exception {
        AddressSet seen = new AddressSet();
        ArrayDeque<Address> work = new ArrayDeque<>();
        work.add(entry);
        int count = 0;
        boolean sawTerminator = false;
        while (!work.isEmpty()) {
            Address a = work.poll();
            while (true) {
                if (seen.contains(a)) {
                    break;
                }
                Instruction ins = getInstructionAt(a);
                if (ins == null) {
                    if (debugLeft-- > 0) {
                        byte[] db = new byte[12];
                        currentProgram.getMemory().getBytes(a, db);
                        StringBuilder sb = new StringBuilder();
                        for (byte x : db) {
                            sb.append(String.format("%02x ", x));
                        }
                        Instruction pv = getInstructionBefore(a);
                        println("DEBUG nullins entry=" + entry + " at=" + a + " bytes=" + sb + " prev=" + (pv == null ? "null" : pv.getAddress() + " " + pv + " " + pv.getFlowType()));
                    }
                    return fail("nullins");
                }
                if (BAD.contains(ins.getMnemonicString().toUpperCase()) && !ins.getMnemonicString().equalsIgnoreCase("INT3")) {
                    return fail("badmnem");
                }
                if (++count > MAX_INSNS) {
                    return fail("toomany");
                }
                if (ins.getLength() == 2 && ins.getByte(0) == 0 && ins.getByte(1) == 0) {
                    return fail("zero00");
                }
                if (!ins.getFlowType().isComputed()) {
                    for (Address t : ins.getFlows()) {
                        long o = t.getOffset();
                        if (o < REGION_START || o > REGION_END) {
                            return fail("flowout");
                        }
                    }
                }
                seen.add(ins.getMinAddress(), ins.getMaxAddress());
                FlowType ft = ins.getFlowType();
                if (!ft.isCall()) {
                    for (Address t : ins.getFlows()) {
                        long o = t.getOffset();
                        if (o < REGION_START || o > REGION_END) {
                            continue;
                        }
                        // Far jumps are tail calls to other functions, validated on their own.
                        if (gaps.contains(t) && t.getOffset() >= entry.getOffset() - 0x1000
                                && t.getOffset() <= entry.getOffset() + 0x10000) {
                            work.add(t);
                        }
                    }
                }
                if (!ins.hasFallthrough()) {
                    sawTerminator = true;
                    break;
                }
                a = ins.getMaxAddress().add(1);
                if (!gaps.contains(a) && getFunctionContaining(a) == null) {
                    return fail("fallout");
                }
                if (getFunctionContaining(a) != null) {
                    break;
                }
            }
        }
        return sawTerminator ? seen : fail("noterm");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 3;
        String tier = args.length > 1 ? args[1] : "prologue";
        boolean adjacent = tier.equals("adjacent");
        boolean padded = tier.equals("padded") || adjacent;
        boolean evidenceAny = padded || tier.equals("evidence");
        for (int round = 1; round <= rounds; round++) {
            println("round " + round + ": orphan instructions cleared: " + clearOrphanInstructions());
            AddressSet gaps = computeGaps();
            if (round == 1) {
                println("pointer runs defined: " + definePointerRuns(gaps));
                gaps = computeGaps();
            }
            Set<Long> ptrs = collectPointerTargets(gaps);
            Set<Long> calls = collectCallTargets(gaps);
            println("round " + round + ": gap bytes=" + gaps.getNumAddresses()
                    + " ptr targets=" + ptrs.size() + " call targets=" + calls.size());
            List<Long> cands = new ArrayList<>();
            for (AddressRange r : gaps) {
                byte[] buf = new byte[(int) r.getLength()];
                currentProgram.getMemory().getBytes(r.getMinAddress(), buf);
                long lo = r.getMinAddress().getOffset();
                for (int i = 0; i < buf.length; i++) {
                    int s = prologueStrength(buf, i);
                    if (s == 0) {
                        continue;
                    }
                    long a = lo + i;
                    boolean evidence = ptrs.contains(a) || calls.contains(a);
                    if (s == 2 || evidence) {
                        cands.add(a);
                    }
                }
                if (evidenceAny) {
                    for (int i = 0; i < buf.length; i++) {
                        long a = lo + i;
                        if (prologueStrength(buf, i) == 0 && (ptrs.contains(a) || calls.contains(a))) {
                            cands.add(a);
                        }
                    }
                }
                if (adjacent) {
                    for (int i = 0; i < buf.length; i++) {
                        boolean afterRet = i >= 1 && (buf[i - 1] & 0xff) == 0xC3
                                || i >= 3 && (buf[i - 3] & 0xff) == 0xC2 && buf[i - 1] == 0;
                        if ((i == 0 || afterRet) && (buf[i] & 0xff) != 0xCC && (buf[i] & 0xff) != 0) {
                            cands.add(lo + i);
                        }
                    }
                }
                if (padded) {
                    for (int i = 1; i < buf.length; i++) {
                        if (i >= 2 && (buf[i] & 0xff) != 0xCC && (buf[i - 1] & 0xff) == 0xCC
                                && (buf[i - 2] & 0xff) == 0xCC) {
                            cands.add(lo + i);
                        }
                    }
                }
            }
            int created = 0;
            int rejected = 0;
            for (Long c : cands) {
                Address a = addr(c);
                if (getFunctionContaining(a) != null || getInstructionContaining(a) != null) {
                    continue;
                }
                if (!disassemble(a)) {
                    reasons.merge("disassemble-false", 1, Integer::sum);
                    rejected++;
                    continue;
                }
                AddressSet ok = validate(a, gaps);
                if (ok == null) {
                    // Roll back: remove instructions reachable from the entry inside the gap.
                    AddressSet bad = new AddressSet();
                    ArrayDeque<Address> w = new ArrayDeque<>();
                    w.add(a);
                    int guard = 0;
                    while (!w.isEmpty() && guard++ < MAX_INSNS) {
                        Address x = w.poll();
                        Instruction ins = getInstructionAt(x);
                        if (ins == null || bad.contains(x) || !gaps.contains(x)) {
                            continue;
                        }
                        bad.add(ins.getMinAddress(), ins.getMaxAddress());
                        for (Address t : ins.getFlows()) {
                            w.add(t);
                        }
                        if (ins.hasFallthrough()) {
                            w.add(ins.getMaxAddress().add(1));
                        }
                    }
                    if (debugRej-- > 0) {
                        println("DEBUG reject " + a + " " + lastReason);
                    }
                    clearListing(bad);
                    rejected++;
                    continue;
                }
                Function f = createFunction(a, null);
                if (f == null) {
                    clearListing(ok);
                    rejected++;
                } else {
                    created++;
                }
            }
            println("round " + round + ": candidates=" + cands.size() + " created=" + created
                    + " rejected=" + rejected);
            if (created == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }
        println("reject reasons: " + reasons);
        println("FindPrologues: functions = " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
