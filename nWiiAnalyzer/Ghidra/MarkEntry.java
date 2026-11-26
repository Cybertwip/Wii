// Seeds Ghidra headless analysis at the DOL entry point.
// @category WiiRecomp

import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;

public class MarkEntry extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("MarkEntry: missing entry address");
            return;
        }
        long off = Long.parseUnsignedLong(args[0], 16);
        Address entry = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(off);
        try {
            currentProgram.getSymbolTable().addExternalEntryPoint(entry);
        } catch (Exception e) {
            println("entry point: " + e.getMessage());
        }
        try {
            currentProgram.getSymbolTable().createLabel(entry, "nwii_entry",
                    SourceType.USER_DEFINED);
        } catch (Exception e) {
            println("label: " + e.getMessage());
        }
        DisassembleCommand dis = new DisassembleCommand(entry, null, true);
        if (!dis.applyTo(currentProgram, monitor)) {
            println("Disassemble failed: " + dis.getStatusMsg());
        } else {
            println("Disassembled from " + entry);
        }
    }
}
