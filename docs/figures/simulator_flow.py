"""Flowchart for the report: how the Kites simulator runs a program, from clicking Run to the
end of the run (single-cycle VM, rvss). Each block names the step, says what it does in plain
words, and tags the function and source file that implement it.

writes simulator_flow.png and simulator_flow.pdf next to this file.
usage: python docs/figures/simulator_flow.py   (needs matplotlib)
"""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Polygon  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
INK, INK2, LINE, PAGE = "#1d1d1b", "#45443f", "#5c5b56", "#ffffff"
MONO = "DejaVu Sans Mono"
GUI = ("#e3eefb", "#2a78d6")
ASM = ("#fdecdf", "#c85a14")
MGR = ("#ece8f8", "#6c55c7")
CPU = ("#e2f3ec", "#1f8a5b")
VEC = ("#fbe3ea", "#c23a63")
MEM = ("#f1efe8", "#6f6c60")
plt.rcParams.update({"font.family": "DejaVu Sans", "text.color": INK})

W, H, INCHES = 102, 136, 10.2
fig = plt.figure(figsize=(INCHES, INCHES * H / W))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, W)
ax.set_ylim(H, 0)
ax.axis("off")
BH = 10.2  # standard block height


def rect(x, y, w, h, colors, lw=1.3, r=1.0):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0,rounding_size={r}",
                                fc=colors[0], ec=colors[1], lw=lw, zorder=2))


def block(x, y, w, title, tag, desc, colors, h=BH):
    """Step name, the code that implements it (small tag), and what it does."""
    rect(x, y, w, h, colors)
    ax.text(x + 1.3, y + 1.0, title, fontsize=10, fontweight="bold", va="top", zorder=3)
    ax.text(x + 1.3, y + 3.5, tag, family=MONO, fontsize=7.2, color=colors[1], va="top",
            zorder=3)
    ax.text(x + 1.3, y + 5.6, desc, fontsize=8.4, color=INK2, va="top", linespacing=1.4,
            zorder=3)


def arrow(pts, lw=1.3, color=LINE):
    xs, ys = zip(*pts)
    ax.plot(xs, ys, color=color, lw=lw, zorder=1, solid_capstyle="butt")
    ax.annotate("", xy=pts[-1], xytext=pts[-2], zorder=1,
                arrowprops=dict(arrowstyle="-|>,head_length=0.55,head_width=0.3", color=color,
                                lw=lw, shrinkA=0, shrinkB=0))


def down(x, y0, y1):
    arrow([(x, y0), (x, y1)])


def heading(x, y, n, s, colors):
    ax.text(x, y, f"{n}  {s}", fontsize=10.8, fontweight="bold", color=colors[1], va="center")


# ---------------------------------------------------------------------------- title
ax.text(2, 4.2, "How the Kites simulator runs a program", fontsize=15, fontweight="bold",
        va="center")

# ---------------------------------------------------------------------------- left column
LX, LW = 2, 46
LC = LX + LW / 2

heading(LX, 10.5, "1", "Start", GUI)
block(LX, 13, LW, "Start the run", "MainWindow::run()  -  mainwindow.cpp",
      "The user clicks Run. The editor is locked so the\ncode cannot change while it runs.", GUI)
block(LX, 25.2, LW, "Prepare the source code", "tryParseAndLoadProgram()  -  mainwindow.cpp",
      "The editor text is collected, user-defined pseudo-\ninstructions are expanded, and the "
      "file is saved.", GUI)
down(LC, 23.2, 25.2)

heading(LX, 38.5, "2", "Assemble", ASM)
down(LC, 35.4, 41)
block(LX, 41, LW, "Assemble the program", "assemble()  -  assembler.cpp",
      "Runs the three stages below. If a line has an error,\nthe run stops and the editor "
      "marks that line.", ASM)
block(LX, 53.2, LW, "Split into tokens", "Lexer  -  lexer.cpp",
      "Breaks the text into instruction names, registers,\nnumbers and labels.", ASM)
block(LX, 65.4, LW, "Check and resolve", "Parser  -  parser.cpp",
      "Checks each line against its instruction format,\nrecords label addresses and expands "
      "pseudo-instructions.", ASM)
block(LX, 77.6, LW, "Encode", "Code generator  -  code_generator.cpp",
      "Turns every instruction into its 32-bit machine\ncode word.", ASM)
down(LC, 51.2, 53.2)
down(LC, 63.4, 65.4)
down(LC, 75.6, 77.6)

heading(LX, 91, "3", "Load and start", MGR)
down(LC, 87.8, 93.5)
block(LX, 93.5, LW, "Load into memory", "LoadProgram()  -  processor_base.cpp",
      "The machine code is placed in memory from address 0,\nthe data section from "
      "0x10000000.", MGR)
block(LX, 105.7, LW, "Start the processor", "runSlot()  -  processor_manager.cpp",
      "The simulated processor starts on its own thread,\nso the window stays responsive.",
      MGR)
down(LC, 103.7, 105.7)

heading(LX, 120.5, "5", "Finish", MEM)
block(LX, 123, LW, "End of the program", "Run()  ->  runFinishedSlot()",
      "The final register and memory state is saved, and\nthe editor and toolbar are "
      "unlocked again.", MEM)

# ---------------------------------------------------------------------------- right column
RX, RW = 52, 46
RC = RX + RW / 2
heading(RX, 10.5, "4", "Execute, one instruction at a time", CPU)

# entry from step 3: through the gap, over the top, into the decision
arrow([(LX + LW, 110.8), (50, 110.8), (50, 13.2), (83, 13.2), (83, 14.6)])
DX, DY = 83, 19.4
ax.add_patch(Polygon([(DX, DY - 4.8), (DX + 14.5, DY), (DX, DY + 4.8), (DX - 14.5, DY)],
                     closed=True, fc=CPU[0], ec=CPU[1], lw=1.3, zorder=2))
ax.text(DX, DY, "More instructions\nto run?", ha="center", va="center", fontsize=8.8,
        fontweight="bold", zorder=3, linespacing=1.25)
rect(RX + 1, 16.6, 14.5, 5.6, MEM, lw=1.0)
ax.text(RX + 8.25, 19.4, "No: go to 5", ha="center", va="center", fontsize=8.4,
        fontweight="bold", zorder=3)
arrow([(DX - 14.5, DY), (RX + 15.5, DY)])
ax.text(DX + 1, 25.9, "Yes", fontsize=8, color=INK2, style="italic", va="center")
ax.text(RX + 1, 24.6, "pauses here at a breakpoint\nor when Pause is pressed", fontsize=7.4,
        color=INK2, style="italic", va="top")
down(DX, DY + 4.8, 28)

block(RX, 28, RW, "Fetch", "Fetch()  -  rvss_processor.cpp",
      "Reads the instruction at the program counter (PC)\nfrom memory and moves the PC to the "
      "next one.", CPU)
block(RX, 40.2, RW, "Decode", "Decode()  -  rvss_control_unit.cpp",
      "Identifies the instruction and sets the control\nsignals: which registers, ALU "
      "operation, memory use.", CPU)
down(RC, 38.2, 40.2)
down(RC, 50.4, 52.4)

# Execute: one of three units
EY, EH = 52.4, 34.2
rect(RX, EY, RW, EH, CPU)
ax.text(RX + 1.3, EY + 1.0, "Execute", fontsize=10, fontweight="bold", va="top", zorder=3)
ax.text(RX + 1.3, EY + 3.5, "Execute()  -  rvss_processor.cpp", family=MONO, fontsize=7.2,
        color=CPU[1], va="top", zorder=3)
ax.text(RX + 1.3, EY + 5.6, "Sends the instruction to the unit that handles it:", fontsize=8.4,
        color=INK2, va="top", zorder=3)


def unit(y, name, tag, desc, colors):
    rect(RX + 1.5, y, RW - 3, 8.0, ("#ffffff", colors[1]), lw=1.0, r=0.8)
    rect(RX + 2.5, y + 0.9, 13.5, 2.9, colors, lw=0.9, r=0.6)
    ax.text(RX + 9.25, y + 2.35, name, ha="center", va="center", fontsize=8.2,
            fontweight="bold", zorder=3)
    ax.text(RX + 17.2, y + 2.35, tag, family=MONO, fontsize=7.0, color=colors[1], va="center",
            zorder=3)
    ax.text(RX + 2.7, y + 4.5, desc, fontsize=8.0, color=INK, va="top", linespacing=1.35,
            zorder=3)


unit(EY + 8.6, "Scalar ALU", "alu.cpp",
     "Arithmetic, logic and comparisons on registers;\nbranches and jumps choose the next PC.",
     CPU)
unit(EY + 17.2, "Vector unit", "processor/vector/",
     "Sets the vector length, loads or stores whole\nvectors, or applies one operation to all "
     "elements.", VEC)
unit(EY + 25.8, "Other units", "rvss_processor.cpp",
     "System calls (print, exit), floating-point and\ncontrol/status register instructions.",
     CPU)

block(RX, 88.6, RW, "Memory access", "WriteMemory()  -  rvss_processor.cpp",
      "Loads and stores read or write data memory\nthrough the L1 and L2 caches.", CPU)
block(RX, 100.8, RW, "Write back", "WriteBack()  -  rvss_processor.cpp",
      "The result is written to the destination register.", CPU, h=8.6)
block(RX, 111.4, RW, "Update the display", "Qt signals  ->  GUI tabs",
      "The current line is highlighted and the register,\nvector and memory views refresh; "
      "then the\nsimulator waits for the chosen speed.", GUI, h=11.8)
down(RC, 86.6, 88.6)
down(RC, 98.8, 100.8)
down(RC, 109.4, 111.4)
# loop back to the decision
arrow([(RX + RW, 117.3), (100.3, 117.3), (100.3, DY), (DX + 14.5, DY)])
ax.text(101.3, 68, "next instruction", rotation=90, fontsize=7.8, color=INK2, style="italic",
        ha="center", va="center")


for ext in ("png", "pdf"):
    fig.savefig(os.path.join(HERE, f"simulator_flow.{ext}"), dpi=240, facecolor=PAGE)
print("written simulator_flow.png / .pdf to", HERE)
