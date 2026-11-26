// Writes Name,Location,Size for every function. Headless: no askFile dialog.
// Args: <out.csv> [entryHex]
// @category WiiRecomp

import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.File;
import java.io.PrintWriter;

public class ExportSymbolsHeadless extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("ExportSymbolsHeadless: missing output csv");
            return;
        }
        File out = new File(args[0]);
        Address entry = null;
        if (args.length >= 2 && !args[1].isEmpty()) {
            long off = Long.parseUnsignedLong(args[1], 16);
            entry = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(off);
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        if (fm.getFunctionCount() == 0 && entry != null) {
            println("No functions from analysis; creating them from the entry disassembly");
            DisassembleCommand dis = new DisassembleCommand(entry, null, true);
            dis.applyTo(currentProgram, monitor);
            new CreateFunctionCmd(entry).applyTo(currentProgram, monitor);
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction ins = it.next();
                if (!ins.getFlowType().isCall()) {
                    continue;
                }
                for (Address dest : ins.getFlows()) {
                    if (fm.getFunctionAt(dest) == null) {
                        new CreateFunctionCmd(dest).applyTo(currentProgram, monitor);
                    }
                }
            }
        }

        PrintWriter writer = new PrintWriter(out);
        writer.println("Name,Location,Size");
        int n = 0;
        FunctionIterator funcs = fm.getFunctions(true);
        while (funcs.hasNext()) {
            if (monitor.isCancelled()) {
                break;
            }
            Function f = funcs.next();
            long addr = f.getEntryPoint().getOffset();
            long size = f.getBody().getNumAddresses();
            String name = f.getName().replace(",", "_");
            writer.printf("%s,%08X,%d%n", name, addr, size);
            n++;
        }
        writer.close();
        println("Exported " + n + " symbols to " + out.getAbsolutePath());
    }
}
