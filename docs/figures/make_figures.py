"""Draws the overview flowchart for the report (needs matplotlib):

  execution_flow.png/.pdf        what happens, step by step, from typing a program to the end of Run

usage: python docs/figures/make_figures.py
"""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Polygon, Rectangle  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

INK, INK2, MUTED, LINE = "#1d1d1b", "#4a4945", "#7a7973", "#5c5b56"
PAGE = "#ffffff"
# one soft fill + a darker edge per layer
GUI = ("#e3eefb", "#2a78d6")
ASM = ("#fdecdf", "#d9661f")
MGR = ("#ece8f8", "#6c55c7")
CPU = ("#e2f3ec", "#1f8a5b")
VEC = ("#fbe3ea", "#c23a63")
MEM = ("#f1efe8", "#7d7a6e")
ERR = ("#fde8e6", "#c8372d")

plt.rcParams.update({"font.family": "DejaVu Sans", "text.color": INK})


class Canvas:
    """Drawing in a fixed coordinate system: x 0..W, y 0..H with y growing downwards."""

    def __init__(self, w, h, inches_w):
        self.W, self.H = w, h
        self.fig = plt.figure(figsize=(inches_w, inches_w * h / w))
        self.ax = self.fig.add_axes([0, 0, 1, 1])
        self.ax.set_xlim(0, w)
        self.ax.set_ylim(h, 0)
        self.ax.axis("off")
        self.fig.patch.set_facecolor(PAGE)

    def box(self, x, y, w, h, title, body=None, colors=MEM, title_size=10, body_size=8.4,
            align="center", radius=1.2, lw=1.3, dashed=False, title_color=None, header=False):
        fill, edge = colors
        self.ax.add_patch(FancyBboxPatch(
            (x, y), w, h, boxstyle=f"round,pad=0,rounding_size={radius}", fc=fill, ec=edge,
            lw=lw, ls=(0, (4, 2)) if dashed else "-", zorder=2))
        tx = x + w / 2 if align == "center" else x + 1.4
        if header:
            self.ax.text(tx, y + 1.0, title, ha=align, va="top", fontsize=title_size,
                         fontweight="bold", color=title_color or INK, zorder=3)
        elif body:
            self.ax.text(tx, y + 1.2, title, ha=align, va="top", fontsize=title_size,
                         fontweight="bold", color=title_color or INK, zorder=3)
            self.ax.text(tx, y + 1.2 + title_size * 0.27, body, ha=align, va="top",
                         fontsize=body_size, color=INK2, linespacing=1.35, zorder=3)
        else:
            self.ax.text(tx, y + h / 2, title, ha=align, va="center", fontsize=title_size,
                         fontweight="bold", color=title_color or INK, zorder=3)

    def diamond(self, cx, cy, w, h, text, colors=MGR, size=8.6):
        fill, edge = colors
        self.ax.add_patch(Polygon([(cx, cy - h / 2), (cx + w / 2, cy), (cx, cy + h / 2),
                                   (cx - w / 2, cy)], closed=True, fc=fill, ec=edge, lw=1.3,
                                  zorder=2))
        self.ax.text(cx, cy, text, ha="center", va="center", fontsize=size, fontweight="bold",
                     color=INK, zorder=3, linespacing=1.25)

    def arrow(self, pts, label=None, label_at=0, label_dx=0.8, label_dy=0, color=LINE,
              dashed=False, lw=1.3, head=True, label_ha="left", size=8):
        xs, ys = zip(*pts)
        ls = (0, (4, 2.5)) if dashed else "-"
        self.ax.plot(xs[:-1] + (xs[-1],), ys, color=color, lw=lw, ls=ls, zorder=1,
                     solid_capstyle="butt")
        if head:
            self.ax.annotate("", xy=pts[-1], xytext=pts[-2],
                             arrowprops=dict(arrowstyle="-|>,head_length=0.55,head_width=0.3",
                                             color=color, lw=lw, shrinkA=0, shrinkB=0),
                             zorder=1)
        if label:
            (x0, y0), (x1, y1) = pts[label_at], pts[label_at + 1]
            self.ax.text((x0 + x1) / 2 + label_dx, (y0 + y1) / 2 + label_dy, label,
                         ha=label_ha, va="center", fontsize=size, color=INK2, zorder=4,
                         style="italic",
                         bbox=dict(fc=PAGE, ec="none", pad=0.6) if label_ha == "center" else None)

    def text(self, x, y, s, size=9, color=INK2, **kw):
        self.ax.text(x, y, s, fontsize=size, color=color, zorder=4, **kw)

    def save(self, name):
        for ext in ("png", "pdf"):
            self.fig.savefig(os.path.join(HERE, f"{name}.{ext}"), dpi=220, facecolor=PAGE)
        plt.close(self.fig)


# --------------------------------------------------------------------------------------------
# Figure 2: execution flow
# --------------------------------------------------------------------------------------------
def execution():
    c = Canvas(100, 156, 11)
    c.text(2, 3, "How a program runs in Kites, step by step", size=15, color=INK,
           fontweight="bold", va="center")
    c.text(2, 6.2, "From typing the assembly to the end of Run in the GUI (the CLI follows the "
           "same path from step 3 on).", size=9.5, va="center")

    X, BW = 13, 42          # main column
    CX = X + BW / 2

    def phase(y0, y1, n, name, colors):
        c.ax.add_patch(Rectangle((1.5, y0), 9.5, y1 - y0, fc=colors[0], ec="none", zorder=0))
        c.ax.add_patch(Rectangle((1.5, y0), 0.7, y1 - y0, fc=colors[1], ec="none", zorder=0))
        c.text(6.6, (y0 + y1) / 2, f"{n}\n{name}", size=9.5, color=colors[1],
               fontweight="bold", ha="center", va="center", rotation=90, linespacing=1.3)

    phase(9, 32, 1, "Write & Run", GUI)
    phase(32.5, 74, 2, "Assemble", ASM)
    phase(75.5, 92, 3, "Load", MGR)
    phase(93.5, 143, 4, "Execute loop", CPU)
    phase(144.5, 154, 5, "Finish", MEM)

    def step(y, h, title, body, colors, code=None):
        c.box(X, y, BW, h, title, body, colors=colors, align="left", title_size=9.6,
              body_size=8)
        if code:
            c.text(X + BW - 1.2, y + 1.3, code, size=7.2, color=colors[1], ha="right",
                   va="top", family="DejaVu Sans Mono")

    # 1. write & run
    step(10, 6.4, "User writes the assembly program", "in the Editor tab (.text / .data, "
         "labels, pseudo-instructions)", GUI, "EditorTab")
    step(18.5, 5.4, "User clicks Run", "toolbar action -> MainWindow::run()", GUI,
         "MainWindow")
    step(25.6, 5.8, "Prepare the source", "expand custom pseudo-instructions; save a temp .s file",
         GUI, "customPseudoManager")
    c.arrow([(CX, 16.4), (CX, 18.5)])
    c.arrow([(CX, 23.9), (CX, 25.6)])
    c.arrow([(CX, 31.4), (CX, 33.5)])

    # 2. assemble
    step(33.5, 7.4, "Lexer", "splits the text into tokens: mnemonics, registers,\n"
         "immediates, labels, directives", ASM, "lexer.cpp")
    step(43, 10, "Parser", "checks each line against its instruction format, records\n"
         "label addresses (symbol table), expands pseudo-\ninstructions (li, la, ...) and "
         "collects .data items", ASM, "parser.cpp")
    step(55, 6.4, "Code generator", "encodes every instruction as a 32-bit machine word",
         ASM, "code_generator.cpp")
    c.arrow([(CX, 40.9), (CX, 43)])
    c.arrow([(CX, 53), (CX, 55)])
    c.arrow([(CX, 61.4), (CX, 63)])
    c.diamond(CX, 65.4, 16, 4.8, "Errors?", colors=ASM)
    c.box(62, 62.6, 30, 5.6, "Stop: error lines marked in the editor", None, colors=ERR,
          title_size=8.6)
    c.arrow([(CX + 8, 65.4), (62, 65.4)], "yes", label_dx=-0.6, label_dy=-1.1,
            label_ha="center")
    step(69, 4.4, "AssembledProgram", None, ASM)
    c.text(X + 18, 71.2, "text_buffer, data_buffer, line mappings", size=8, va="center")
    c.arrow([(CX, 67.8), (CX, 69)], "no", label_dx=1, label_dy=0)
    c.box(62, 69, 30, 4.4, "Disassembly view filled in", None, colors=("#ffffff", GUI[1]),
          title_size=8.6)
    c.arrow([(X + BW, 71.2), (62, 71.2)], lw=1.1)

    # 3. load
    step(76.5, 8.6, "Load into the VM", "ProcessorManager::loadProgram -> LoadProgram:\n"
         ".text words -> memory from 0x0, .data -> 0x10000000;\neditor breakpoints set",
         MGR, "processor_base.cpp")
    step(86.6, 5.6, "Start the VM thread", "runProcessorSignal -> runSlot() -> Run()", MGR,
         "QThread")
    c.arrow([(CX, 73.4), (CX, 76.5)])
    c.arrow([(CX, 85.1), (CX, 86.6)])
    c.arrow([(CX, 92.2), (CX, 94.2)])

    # 4. execute loop
    c.diamond(CX, 97.6, 30, 6.8, "PC < end of program\nand not stopped?", colors=CPU)
    c.diamond(CX, 107.2, 26, 6, "Breakpoint or Pause?", colors=CPU, size=8.4)
    c.arrow([(CX, 101), (CX, 104.2)], "yes", label_dx=-4.2)
    c.arrow([(CX + 13, 107.2), (CX + 17, 107.2), (CX + 17, 102.4), (CX, 102.4)], lw=1.1)
    c.text(CX + 17.8, 105.2, "yes: wait until\nResume / Step", size=7.8, va="center",
           style="italic")
    step(113, 10, "Step(): execute one instruction", "Fetch, Decode, Execute, Memory, "
         "WriteBack\n(see the panel on the right); store an undo record;\n"
         "instructions_retired += 1", CPU, "rvss_processor.cpp")
    c.arrow([(CX, 110.2), (CX, 113)], "no", label_dx=1)
    step(125.5, 8.6, "Update the GUI", "processorClockedSignal + RegisterFile signals:\n"
         "editor highlight, register table, Vector tab,\nMemory tab, datapath view", CPU,
         "Qt signals")
    step(136.2, 5.8, "Wait for the step delay", "speed set in the toolbar (0 ms in the CLI)",
         CPU)
    c.arrow([(CX, 123), (CX, 125.5)])
    c.arrow([(CX, 134.1), (CX, 136.2)])
    # loop back
    c.arrow([(X, 138.9), (X - 1.2, 138.9), (X - 1.2, 97.6), (CX - 15, 97.6)], lw=1.2)
    c.text(X - 1.0, 120, "next instruction", size=7.8, rotation=90, ha="right", va="center",
           style="italic", color=INK2)

    # 5. finish
    c.arrow([(CX + 15, 97.6), (60, 97.6), (60, 149), (X + BW, 149)], "no: program finished",
            label_at=0, label_dx=-3.4, label_dy=-1.2, size=7.8, label_ha="left")
    step(145.5, 7.4, "Finish", "dump registers and state (vm_state/*.json);\n"
         "runFinishedSignal -> editor unlocked, buttons reset", MEM, "runFinishedSlot")

    # ---- right panel: inside Step()
    PX, PW = 64, 34.5
    c.box(PX, 76.5, PW, 66, "Inside Step()  (single-cycle VM)", None, colors=("#fafaf7", LINE),
          title_size=10, dashed=True, radius=1.5)
    c.ax.texts[-1].set_position((PX + PW / 2, 79.2))
    c.arrow([(X + BW, 118), (PX, 118)], dashed=True, lw=1.1, head=False)

    def pbox(y, h, title, body, colors=("#ffffff", CPU[1]), header=False):
        c.box(PX + 1.5, y, PW - 3, h, title, body, colors=colors, align="left", title_size=8.8,
              body_size=7.6, lw=1.0, header=header)

    pbox(82, 6.6, "Fetch", "instruction = memory[PC]   (via\ninstruction cache);  PC += 4")
    pbox(90.6, 6.6, "Decode", "control unit sets RegWrite, ALUSrc,\nMemRead, MemWrite, Branch")
    pbox(99.2, 23.6, "Execute: chosen by opcode", None, header=True)
    rows = [("ecall", "syscall handler (print, exit, ...)", CPU),
            ("F / D", "floating-point unit", CPU),
            ("CSR", "csrrw / csrrs / ... on the CSR file", CPU),
            ("vector", "rvv::ExecuteVectorInstruction", VEC),
            ("other", "ALU result; branch / jump -> next PC", CPU)]
    for i, (k, v, col) in enumerate(rows):
        y = 104 + i * 3.6
        c.box(PX + 3, y, 7.2, 2.8, k, None, colors=col, title_size=7.6, radius=0.6, lw=0.9)
        c.text(PX + 11.2, y + 1.4, v, size=7.5, va="center")
    pbox(124.6, 7.2, "Vector path", "vle/vse -> vector_memory;  vsetvli -> vl, vtype;\n"
         "arith / reduction / mask / permute ->\nloop over vl elements (v0 mask)",
         colors=("#ffffff", VEC[1]))
    c.ax.texts[-1].set_fontsize(7.1)
    pbox(133.4, 4.2, "Memory", None)
    c.text(PX + 12, 135.5, "scalar load / store -> L1 -> L2 -> RAM", size=7.3, va="center")
    pbox(139.0, 3, "Write back", None)
    c.text(PX + 13.5, 140.5, "rd <- result (RegisterFile)", size=7.3, va="center")
    for y0, y1 in ((88.6, 90.6), (97.2, 99.2), (122.8, 124.6), (131.8, 133.4), (137.6, 139.0)):
        c.arrow([(PX + PW / 2, y0), (PX + PW / 2, y1)], lw=1.0)

    c.text(PX, 145.8, "Pipelined VMs (rv5-*): the same five stages run\noverlapped, one "
           "instruction per stage per clock,\nwith hazard detection and forwarding when "
           "enabled.", size=7.8, va="top", style="italic")
    c.save("execution_flow")


if __name__ == "__main__":
    execution()
    print("written to", HERE)
