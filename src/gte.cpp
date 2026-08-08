#include "SDL_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <time.h>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <algorithm>
#include <cctype>
#ifdef WASM_BUILD
#include "emscripten.h"
#include <emscripten/html5.h>
#else
#include "tinyfd/tinyfiledialogs.h"
#endif

#include "joystick_adapter.h"
#include "audio_coprocessor.h"
#include "blitter.h"
#include "palette.h"

#include "timekeeper.h"
#include "system_state.h"
#include "emulator_config.h"
#include "game_config.h"

#include "mos6502/mos6502.h"

#include "devtools/memory_map.h"
#include "devtools/breakpoints.h"
#include "devtools/source_map.h"

#include "ui/ui_utils.h"
#include "devtools/profiler.h"
#include "devtools/disassembler.h"

#ifndef WASM_BUILD
#include "devtools/profiler_window.h"
#include "devtools/mem_browser_window.h"
#include "devtools/vram_window.h"
#include "devtools/stepping_window.h"
#include "devtools/patching_window.h"
#include "devtools/controller_options_window.h"
#include "imgui.h"
#include "implot.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include "whereami/whereami.h"
#include "toml/toml.hpp"
#include "joystick_config.h"
#include "stb/stb_image.h"
#ifdef WRAPPER_MODE
#include "data/proggy_tiny_ttf.h"
#include "data/list_icon.h"
#include "data/settings_icon.h"
#include "data/controller_icon.h"
#endif
#endif

#ifndef WINDOW_TITLE
#define WINDOW_TITLE "GameTank Emulator"
#endif

using namespace std;

const int GT_WIDTH = 128;
const int GT_HEIGHT = 128;
const int VIEWPORT_ASPECT_W = 4;
const int VIEWPORT_ASPECT_H = 3;
const int MIN_DISPLAY_SCALE = 2;
#ifdef WRAPPER_MODE
int display_scale = 2; // CRT: fixed 1x/2x, default 2x (not fitted to window)
int imageOffsetX = 0;
int imageOffsetY = 0;
#else
int display_scale = 4;
#endif

int viewport_width(int scale) {
	return (GT_HEIGHT * scale * VIEWPORT_ASPECT_W + VIEWPORT_ASPECT_H - 1) / VIEWPORT_ASPECT_H;
}

int viewport_height(int scale) {
	return GT_HEIGHT * scale;
}

RomType loadedRomType;

mos6502 *cpu_core;
Blitter *blitter;
AudioCoprocessor *soundcard;
JoystickAdapter *joysticks;
SystemState system_state;
CartridgeState cartridge_state;

RGB_Color *palette;

MemoryMap* loadedMemoryMap;
GameConfig* gameconfig;
std::string currentRomFilePath;
std::string nvramFileFullPath;
std::string flashFileFullPath;

bool vsyncProfileArmed = false;
bool vsyncProfileRunning = false;

bool showMenu = false;
bool menuOpening = false;
bool pauseWhenMenuOpen = true;
float menuPanelAlpha = 0.94f;
bool romLoaded = false;
#ifdef WRAPPER_MODE
constexpr int kWrapperTabCount = 3;
enum { TAB_GAMES = 0, TAB_OPTIONS = 1, TAB_CONTROLLER = 2 };

std::vector<std::filesystem::path> wrapperRomList;
int wrapperRomListSelected = 0;
int wrapperForceTab = -1; // tab index, or -1 = none
int wrapperUiTab = 0;     // last selected tab (persists for input routing)
int wrapperSettingsNav = 0;
int wrapperCtrlNav = 0;
int wrapperCtrlCol = 0; // 0 = Set, 1 = Clr on mapping rows
bool wrapperRemapListening = false;
int wrapperRemapTarget = -1; // GameTankButtons::ButtonId, or -2 for Menu/System
bool wrapperRemapConflict = false;
SDL_Texture* wrapperTabIconList = nullptr;
SDL_Texture* wrapperTabIconSettings = nullptr;
SDL_Texture* wrapperTabIconController = nullptr;

struct WrapperMenuInput {
	bool tabLeft = false;
	bool tabRight = false;
	bool left = false;
	bool right = false;
	bool up = false;
	bool down = false;
	bool activate = false;
	bool dec = false;
	bool inc = false;
};

static uint32_t wrapperMenuBtnPrev = 0;
static bool wrapperMenuInputSynced = false;
static bool wrapperSettingsDirty = false;
static Uint32 wrapperMenuHeldSince[9] = {};
static Uint32 wrapperMenuLastRepeat[9] = {};
std::filesystem::path wrapperPendingRom;
bool wrapperRomLoadQueued = false;

void SyncWrapperMenuInput() {
	wrapperMenuInputSynced = false;
	for(int i = 0; i < 9; ++i) {
		wrapperMenuHeldSince[i] = 0;
		wrapperMenuLastRepeat[i] = 0;
	}
}

WrapperMenuInput PollWrapperMenuInput() {
	enum {
		B_UP = 0, B_DOWN, B_LEFT, B_RIGHT,
		B_ACTIVATE, B_DEC, B_INC, B_TAB_L, B_TAB_R, B_COUNT
	};
	uint32_t cur = 0;
	auto put = [&](int bit, bool down) {
		if(down) cur |= (1u << bit);
	};

	SDL_GameController* gc = joysticks ? joysticks->GetGameController() : nullptr;
	if(gc) {
		const Sint16 lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
		const Sint16 ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
		put(B_UP, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -16000);
		put(B_DOWN, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 16000);
		put(B_LEFT, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -16000);
		put(B_RIGHT, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 16000);
		put(B_ACTIVATE, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0);
		put(B_DEC, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X) != 0);
		put(B_INC, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0);
		put(B_TAB_L, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0);
		put(B_TAB_R, SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0);
	}

	const Uint8* keys = SDL_GetKeyboardState(nullptr);
	put(B_UP, keys[SDL_SCANCODE_UP]);
	put(B_DOWN, keys[SDL_SCANCODE_DOWN]);
	put(B_LEFT, keys[SDL_SCANCODE_LEFT]);
	put(B_RIGHT, keys[SDL_SCANCODE_RIGHT]);
	put(B_ACTIVATE, keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] || keys[SDL_SCANCODE_SPACE]);
	put(B_DEC, keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]);
	put(B_INC, keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]);
	{
		const bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
		put(B_TAB_L, keys[SDL_SCANCODE_TAB] && shift);
		put(B_TAB_R, keys[SDL_SCANCODE_TAB] && !shift);
	}

	WrapperMenuInput out{};
	if(!wrapperMenuInputSynced) {
		wrapperMenuBtnPrev = cur;
		wrapperMenuInputSynced = true;
		return out;
	}

	const Uint32 now = SDL_GetTicks();
	constexpr Uint32 kRepeatDelayMs = 350;
	constexpr Uint32 kRepeatRateMs = 45;
	constexpr uint32_t kRepeatMask =
		(1u << B_UP) | (1u << B_DOWN) | (1u << B_LEFT) | (1u << B_RIGHT) |
		(1u << B_DEC) | (1u << B_INC);

	auto fired = [&](int bit) -> bool {
		const uint32_t mask = (1u << bit);
		const bool down = (cur & mask) != 0;
		const bool wasDown = (wrapperMenuBtnPrev & mask) != 0;
		if(down && !wasDown) {
			wrapperMenuHeldSince[bit] = now;
			wrapperMenuLastRepeat[bit] = now;
			return true;
		}
		if(!down) {
			wrapperMenuHeldSince[bit] = 0;
			wrapperMenuLastRepeat[bit] = 0;
			return false;
		}
		if((kRepeatMask & mask) == 0) {
			return false;
		}
		if(now - wrapperMenuHeldSince[bit] < kRepeatDelayMs) {
			return false;
		}
		if(now - wrapperMenuLastRepeat[bit] < kRepeatRateMs) {
			return false;
		}
		wrapperMenuLastRepeat[bit] = now;
		return true;
	};

	out.up = fired(B_UP);
	out.down = fired(B_DOWN);
	out.left = fired(B_LEFT);
	out.right = fired(B_RIGHT);
	out.activate = fired(B_ACTIVATE);
	out.dec = fired(B_DEC);
	out.inc = fired(B_INC);
	out.tabLeft = fired(B_TAB_L);
	out.tabRight = fired(B_TAB_R);
	wrapperMenuBtnPrev = cur;
	return out;
}
#endif
#define GRID_NONE 0
#define GRID_25   1
#define GRID_50   2
#define GRID_FULL 3
int grid_mode = GRID_NONE;
int resetQueued = 0;
#define MUTE_SOURCE_MANUAL 1
#define MUTE_SOURCE_MENU 2
int muteMask = 0;
bool paddle_emulation_enabled = false;
bool paddle_touch_mode = false;

void SaveNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("SAVING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	file.write((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

void LoadNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("LOADING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::in | ios_base::binary);
	file.read((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

std::thread savingThread;

void SaveModifiedFlash() {
	if(EmulatorConfig::noSave) return;
	fstream file_out, file_in;
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	file_in.open(currentRomFilePath, ios_base::in | ios_base::binary);
	file_out.open(flashFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	while(file_in) {
		file_in.read((char*) buf, 256);
		size_t bytesRead = file_in.gcount();
		if(bytesRead) {
			for(int i = 0; i < bytesRead; ++i) {
				buf[i] ^= *(rom_cursor++);
			}
			file_out.write((char*) buf, bytesRead);
		}
	}
	file_in.close();
	file_out.close();
#ifdef WASM_BUILD
	EM_ASM(
		FS.syncfs(false, function (err) {
			assert(!err);
			});
	);
#endif
}

fstream orig_rom, xor_file;
void LoadModifiedFlash() {
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	uint8_t bufx[256];
	size_t bytes_read = 0;
	std::cout << "opening " << currentRomFilePath << " and " << flashFileFullPath << "\n";
	orig_rom.open(currentRomFilePath, ios_base::in | ios_base::binary);
	xor_file.open(flashFileFullPath, ios_base::in | ios_base::binary);
	std::cout << "XORing files together... \n";
	while(orig_rom && xor_file) {
		orig_rom.read((char*) buf, 256);
		xor_file.read((char*) bufx, 256); 
		for(int i = 0; i < orig_rom.gcount(); ++i) {
			*(rom_cursor++) = buf[i] ^ bufx[i];
		}
		bytes_read += 256;
	}
	std::cout << bytes_read << " bytes read from xor file\n";
#ifndef WASM_BUILD
	orig_rom.close();
	xor_file.close();
#endif
}

const uint8_t VIA_ORB    = 0x0;
const uint8_t VIA_ORA    = 0x1;
const uint8_t VIA_DDRB   = 0x2;
const uint8_t VIA_DDRA   = 0x3;
const uint8_t VIA_T1CL   = 0x4;
const uint8_t VIA_T1CH   = 0x5;
const uint8_t VIA_T1LL   = 0x6;
const uint8_t VIA_T1LH   = 0x7;
const uint8_t VIA_T2CL   = 0x8;
const uint8_t VIA_T2CH   = 0x9;
const uint8_t VIA_SR     = 0xA;
const uint8_t VIA_ACR    = 0xB;
const uint8_t VIA_PCR    = 0xC;
const uint8_t VIA_IFR    = 0xD;
const uint8_t VIA_IER    = 0xE;
const uint8_t VIA_ORA_NH = 0xF;

//Pins of VIA Port A used for Serial comms (or other misc cartridge use)
const uint8_t VIA_SPI_BIT_CLK  = 0b00000001;
const uint8_t VIA_SPI_BIT_MOSI = 0b00000010;
const uint8_t VIA_SPI_BIT_CS   = 0b00000100;
const uint8_t VIA_SPI_BIT_MISO = 0b10000000;

#define RAM_HIGHBITS_SHIFT 7

#define FULL_RAM_ADDRESS(x) (((system_state.banking & BANK_RAM_MASK) << RAM_HIGHBITS_SHIFT) | (x))

extern unsigned char font_map[];

Timekeeper timekeeper;
Profiler profiler(timekeeper);

SDL_Surface* gRAM_Surface = NULL;
SDL_Surface* vRAM_Surface = NULL;

SDL_Window* mainWindow = NULL;
SDL_Window* buffers_window = NULL;
Uint32 rmask, gmask, bmask, amask;

#ifndef WASM_BUILD
ImGuiContext* main_imgui_ctx;
ImPlotContext* main_implot_ctx;

std::vector<BaseWindow*> toolWindows;
#endif

SDL_Renderer* mainRenderer = NULL;
SDL_Texture* framebufferTexture = NULL;
SDL_Texture* gridOverlayTexture = NULL;
int gridOverlayScale = 0;

void rebuildGridOverlay(int scale) {
	if(gridOverlayTexture && gridOverlayScale == scale) {
		return;
	}

	if(gridOverlayTexture) {
		SDL_DestroyTexture(gridOverlayTexture);
		gridOverlayTexture = NULL;
	}

	int w = GT_WIDTH * scale;
	int h = GT_HEIGHT * scale;
	gridOverlayTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
	SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);

	SDL_Texture* prev_target = SDL_GetRenderTarget(mainRenderer);
	SDL_SetRenderTarget(mainRenderer, gridOverlayTexture);
	SDL_SetRenderDrawBlendMode(mainRenderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(mainRenderer, 0, 0, 0, 0);
	SDL_RenderClear(mainRenderer);
	SDL_SetRenderDrawColor(mainRenderer, 0, 0, 0, 255);

	SDL_Rect line;
	for(int i = 0; i < GT_WIDTH; ++i) {
		line.x = i * scale;
		line.y = 0;
		line.w = 1;
		line.h = h;
		SDL_RenderFillRect(mainRenderer, &line);
	}
	for(int i = 1; i < GT_HEIGHT; ++i) {
		line.x = 0;
		line.y = i * scale;
		line.w = w;
		line.h = 1;
		SDL_RenderFillRect(mainRenderer, &line);
	}

	SDL_SetRenderTarget(mainRenderer, prev_target);
	SDL_SetTextureScaleMode(gridOverlayTexture, SDL_ScaleModeNearest);
	gridOverlayScale = scale;
}

bool isFullScreen = false;

bool profiler_open = false;
bool buffers_open = false;
int profiler_x_axis = 0;

uint8_t open_bus() {
	return rand() % 256;
}

uint8_t VDMA_Read(uint16_t address) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		return open_bus();
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
			}
		} else {
			bufPtr = system_state.gram;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		return bufPtr[(address & 0x3FFF) | offset];
	}
}

void VDMA_Write(uint16_t address, uint8_t value) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		blitter->SetParam(address, value);
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		SDL_Surface* targetSurface = NULL;
		uint32_t yShift = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			targetSurface = vRAM_Surface;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
				yShift = GT_HEIGHT;
			}
		} else {
			bufPtr = system_state.gram;
			targetSurface = gRAM_Surface;
			yShift = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) * GT_HEIGHT;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		bufPtr[(address & 0x3FFF) | offset] = value;

		uint8_t x, y;
		x = address & 127;
		y = (address >> 7) & 127;
		put_pixel32(targetSurface, x, y + yShift, Palette::ConvertColor(targetSurface, value));
	}
}

void UpdateFlashShiftRegister(uint8_t nextVal) {
	//TODO: Care about DDR bits
	//For now assuming that if we're using Flash2M hardware we're behaving ourselves
	uint8_t oldVal = system_state.VIA_regs[VIA_ORA];
	uint8_t risingBits = nextVal & ~oldVal;
	if(risingBits & VIA_SPI_BIT_CLK) {
		cartridge_state.bank_shifter = cartridge_state.bank_shifter << 1;
		cartridge_state.bank_shifter &= 0xFE;
		cartridge_state.bank_shifter |= !!(oldVal & VIA_SPI_BIT_MOSI);
	} else if(risingBits & VIA_SPI_BIT_CS) {
		//flash cart CS is connected to latch clock
		if((cartridge_state.bank_mask ^ cartridge_state.bank_shifter) & 0x80) {
			SaveNVRAM();
		}
		cartridge_state.bank_mask = cartridge_state.bank_shifter;
		if(loadedRomType != RomType::FLASH2M_RAM32K) {
			cartridge_state.bank_mask |= 0x80;
		}
		//printf("Flash highbits set to %x\n", cartridge_state.bank_mask);
	}
}

uint8_t MemoryRead_Flash2M(uint16_t address) {
	if(address & 0x4000) {
		return cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)];
	} else {
		if(!(cartridge_state.bank_mask & 0x80))
			return cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)];
		else return cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)];
	}
}

uint8_t MemoryRead_Unknown(uint16_t address) {
	//If cartridge_state.size is smaller than unbanked ROM range, align end with 0xFFFF and wrap
	//If cartridge_state.size is bigger than unbanked ROM range, access mainWindow at end of file.
	//TODO: Decide if unknown ROM type should just terminate emulator :P
	if(cartridge_state.size <= 32768) {
		return cartridge_state.rom[((address & 0x7FFF) + 32768 - cartridge_state.size) % cartridge_state.size];
	} else {
		return cartridge_state.rom[((address & 0x7FFF) + cartridge_state.size - 32768)];
	}
}

uint8_t* GetRAM(const uint16_t address) {
	return &(system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)]);
}

uint8_t MemoryReadResolve(const uint16_t address, bool stateful) {
	if(address & 0x8000) {
		switch(loadedRomType) {
			case RomType::EEPROM8K:
			return cartridge_state.rom[address & 0x1FFF];
			case RomType::EEPROM32K:
			return cartridge_state.rom[address & 0x7FFF];
			case RomType::FLASH2M:
			case RomType::FLASH2M_RAM32K:
			return MemoryRead_Flash2M(address);
			case RomType::UNKNOWN:
			return MemoryRead_Unknown(address);
		}
	} else if(address & 0x4000) {
		return VDMA_Read(address);
	} else if((address >= 0x3000) && (address <= 0x3FFF)) {
		return soundcard->ram_read(address);
	} else if((address >= 0x2800) && (address <= 0x2FFF)) {
		return system_state.VIA_regs[address & 0xF];
	} else if(address < 0x2000) {
		if(stateful) {
			if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
				//printf("WARNING! Uninitialized RAM read at %x (Bank %x)\n", address, system_state.banking >> 5);
			}
		}
		return *GetRAM(address);
	} else if((address == 0x2008) || (address == 0x2009)) {
		return joysticks->read((uint8_t) address, stateful);
	}
	if(stateful) {
		printf("Attempted to read write-only device, may be unintended? %x\n", address);
	}
	return open_bus();
}

uint8_t MemoryRead(uint16_t address) {
	return MemoryReadResolve(address, true);
}

uint8_t MemorySync(uint16_t address) {
	if(timekeeper.clock_mode == CLOCKMODE_NORMAL) {
		if(Breakpoints::checkBreakpoint(address, cartridge_state.bank_mask)) {
			timekeeper.clock_mode = CLOCKMODE_STOPPED;
			Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, address, 32);
			cpu_core->Freeze();
		}
		uint8_t opcode = MemoryReadResolve(address, false);
		if(opcode == 0x20) { //JSR
			uint16_t jsr_dest = MemoryReadResolve(address+1, false) | (MemoryReadResolve(address+2, false) << 8);
			profiler.LogJSR(address, cartridge_state.bank_mask, jsr_dest);
		} else if(opcode == 0x60) { //RTS
			profiler.LogRTS(address, cartridge_state.bank_mask);
		}
	}
	return MemoryRead(address);
}

void MemoryWrite(uint16_t address, uint8_t value) {
	if(address & 0x8000) {
		if(loadedRomType == RomType::FLASH2M_RAM32K) {
			if(!(address & 0x4000)) {
				if(!(cartridge_state.bank_mask & 0x80)) {
					cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)] = value;
				}
			}
		}
		if(loadedRomType == RomType::FLASH2M) {
			if(cartridge_state.write_mode) {
				uint8_t* location;
				if(address & 0x4000) {
					location = &(cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)]);
				} else {
					location = &(cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)]);
				}
				*location &= value;
				cartridge_state.write_mode = false;
			} else {
				//Skipping over details like bypass and unlock commands for now
				//So off-spec flash operation will be inaccurate
				if(value == 0x10) {
					//Chip Erase
					for(int i = 0; i < (1 << 21); ++i) {
						cartridge_state.rom[i] = 0xFF;
					}
				} else if (value == 0x30) {
					//Sector erase
					uint8_t sectorBits = ((address & (1 << 13)) >> 13) | ((cartridge_state.bank_mask & 0x7F) << 1);
					uint8_t sectorNum = sectorBits >> 3;
					if(sectorNum < 31) {
						//most of the sector table
						uint32_t x = sectorNum << 16;
						for(uint32_t i = 0; i < (1 << 16); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits & 4) == 0) {
						uint32_t x = 0x1F0000;
						for(uint32_t i = 0; i < (1 << 15); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111100) {
						uint32_t x = 0x1F8000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111101) {
						uint32_t x = 0x1FA000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits >> 1) == 0b1111111) {
						uint32_t x = 0x1FC000;
						for(uint32_t i = 0; i < (1 << 14); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					}
				} else if(value == 0xA0) {
					cartridge_state.write_mode = true;
				} else if(value == 0x90) {
					//first byte of lock command should be a good time to write to file
#ifdef WASM_BUILD
					SaveModifiedFlash();
#else
					if(savingThread.joinable()) {
						savingThread.join();
					}
					savingThread = std::thread(SaveModifiedFlash);
#endif
				}
			}
		}
	}
	else if(address & 0x4000) {
		VDMA_Write(address, value);
	} else if(address >= 0x3000 && address <= 0x3FFF) {
		soundcard->ram_write(address, value);
	} else if((address & 0x2000)) {
		if(address & 0x800) {
			if(loadedRomType == RomType::FLASH2M) {
				if((address & 0xF) == VIA_ORA) {
					UpdateFlashShiftRegister(value);
				}
			}
			if((address & 0xF) == VIA_ORB) {
				if((system_state.VIA_regs[VIA_ORB] & 0x80) && !(value & 0x80)) {
					//falling edge of high bit of ORB
					if(value & 0x40) {
						//report duration
						profiler.LogTime(value & 0x3F);
					} else {
						//store timestamp
						profiler.profilingTimeStamps[value & 0x3F] = timekeeper.totalCyclesCount;
					}
				}
			}
			system_state.VIA_regs[address & 0xF] = value;
		} else {
			if((address & 0x000F) == 0x0007) {
				blitter->CatchUp();
				if((value & DMA_VID_OUT_PAGE_BIT) != (system_state.dma_control & DMA_VID_OUT_PAGE_BIT)) {
					profiler.bufferFlipCount++;
					if(profiler.measure_by_frameflip) {
						profiler.ResetTimers();
						profiler.last_blitter_activity = blitter->pixels_this_frame;
						blitter->pixels_this_frame = 0;
					}
				}
				system_state.dma_control = value;
				system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
				if(system_state.dma_control & DMA_TRANSPARENCY_BIT) {
					SDL_SetColorKey(gRAM_Surface, SDL_TRUE, SDL_MapRGB(gRAM_Surface->format, 0, 0, 0));
				} else {
					SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);
				}
			} else if((address & 0x000F) == 0x0005) {
				blitter->CatchUp();
				system_state.banking = value;
				//printf("banking reg set to %x\n", value);
			} else {
				soundcard->register_write(address, value);
			}
		}
	}
	else if(address < 0x2000) {
		/*if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
			printf("First RAM write at %x (Bank %x) (Value %x)\n", address, system_state.banking >> 6, value);
		}*/
		system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)] = true;
		system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)] = value;
	}
}

SDL_Event e;
bool running = true;
bool gofast = false;
bool paused = true;
bool lshift = false;
bool rshift = false;

void randomize_vram() {
	for(int i = 0; i < VRAM_BUFFER_SIZE; i ++) {
		system_state.vram[i] = rand() % 256;
		put_pixel32(vRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(vRAM_Surface, system_state.vram[i]));
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i ++) {
		system_state.gram[i] = rand() % 256;
		put_pixel32(gRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(gRAM_Surface, system_state.gram[i]));
	}
}

void RefreshPaletteBuffers() {
	if(!vRAM_Surface || !gRAM_Surface) return;
	for(int i = 0; i < VRAM_BUFFER_SIZE; i ++) {
		put_pixel32(vRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(vRAM_Surface, system_state.vram[i]));
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i ++) {
		put_pixel32(gRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(gRAM_Surface, system_state.gram[i]));
	}
}

#ifdef WRAPPER_MODE
static const int kWrapperPalettes[] = {
	PALETTE_SELECT_CAPTURE,
	PALETTE_SELECT_SCALED,
	PALETTE_SELECT_HDMI,
	PALETTE_SELECT_OLD,
};
static const int kWrapperPaletteCount = 4;

extern "C" int LoadRomFile(const char* filename);
extern "C" void PauseEmulation();
extern "C" void ResumeEmulation();

void CycleWrapperPalette(int dir) {
	int idx = 0;
	for(int i = 0; i < kWrapperPaletteCount; ++i) {
		if(kWrapperPalettes[i] == palette_select) {
			idx = i;
			break;
		}
	}
	idx += (dir >= 0) ? 1 : -1;
	if(idx < 0) idx = kWrapperPaletteCount - 1;
	if(idx >= kWrapperPaletteCount) idx = 0;
	palette_select = kWrapperPalettes[idx];
	RefreshPaletteBuffers();
	wrapperSettingsDirty = true;
}

#ifndef PREFS_TITLE
#define PREFS_TITLE "Emulator"
#endif

std::filesystem::path GetWrapperSettingsPath() {
	char* pref = SDL_GetPrefPath("GameTank", PREFS_TITLE);
	std::filesystem::path path = pref ? (std::filesystem::path(pref) / "wrapper_settings.toml") : std::filesystem::path("wrapper_settings.toml");
	if(pref) SDL_free(pref);
	return path;
}

void SaveWrapperSettings() {
	toml::table config;
	config.emplace("menuPanelAlpha", (double)menuPanelAlpha);
	config.emplace("pauseWhenMenuOpen", pauseWhenMenuOpen);
	config.emplace("display_scale", display_scale);
	config.emplace("imageOffsetX", imageOffsetX);
	config.emplace("imageOffsetY", imageOffsetY);
	config.emplace("palette_select", palette_select);
	config.emplace("wrapperUiTab", wrapperUiTab);
	if(AudioCoprocessor::singleton_acp_state) {
		config.emplace("volume", AudioCoprocessor::singleton_acp_state->volume);
	}
	config.emplace("mute", (muteMask & MUTE_SOURCE_MANUAL) != 0);

	const std::filesystem::path path = GetWrapperSettingsPath();
	std::fstream outFile(path, std::ios_base::out | std::ios_base::trunc);
	if(!outFile) {
		printf("Failed to save wrapper settings to %s\n", path.c_str());
		return;
	}
	outFile << config << "\n";
	outFile.close();
	wrapperSettingsDirty = false;
	printf("Saved wrapper settings to %s\n", path.c_str());
}

void LoadWrapperSettings() {
	const std::filesystem::path path = GetWrapperSettingsPath();
	if(!std::filesystem::exists(path)) {
		return;
	}
	try {
		toml::table config = toml::parse_file(path.string());
		menuPanelAlpha = (float)config["menuPanelAlpha"].value_or((double)menuPanelAlpha);
		pauseWhenMenuOpen = config["pauseWhenMenuOpen"].value_or(pauseWhenMenuOpen);
		display_scale = (int)config["display_scale"].value_or(display_scale);
		if(display_scale < 1) display_scale = 1;
		if(display_scale > 2) display_scale = 2;
		imageOffsetX = (int)config["imageOffsetX"].value_or(imageOffsetX);
		imageOffsetY = (int)config["imageOffsetY"].value_or(imageOffsetY);
		imageOffsetX = std::clamp(imageOffsetX, -64, 64);
		imageOffsetY = std::clamp(imageOffsetY, -64, 64);
		palette_select = (int)config["palette_select"].value_or(palette_select);
		wrapperUiTab = (int)config["wrapperUiTab"].value_or(wrapperUiTab);
		wrapperUiTab = std::clamp(wrapperUiTab, 0, kWrapperTabCount - 1);
		if(AudioCoprocessor::singleton_acp_state) {
			AudioCoprocessor::singleton_acp_state->volume = (int)config["volume"].value_or(AudioCoprocessor::singleton_acp_state->volume);
		}
		bool muted = config["mute"].value_or(false);
		if(muted) muteMask |= MUTE_SOURCE_MANUAL;
		else muteMask &= ~MUTE_SOURCE_MANUAL;
		if(AudioCoprocessor::singleton_acp_state) {
			AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
		}
		RefreshPaletteBuffers();
		printf("Loaded wrapper settings from %s\n", path.c_str());
	} catch(const std::exception& e) {
		printf("Failed to load wrapper settings: %s\n", e.what());
	}
}

void MarkWrapperSettingsDirty() {
	wrapperSettingsDirty = true;
}

std::filesystem::path GetExecutableDir() {
	int execPathLength = wai_getExecutablePath(NULL, 0, NULL);
	if(execPathLength == -1) {
		return std::filesystem::current_path();
	}
	char* path = (char*)malloc(execPathLength + 1);
	wai_getExecutablePath(path, execPathLength, NULL);
	path[execPathLength] = '\0';
	std::filesystem::path dir = std::filesystem::path(path).parent_path();
	free(path);
	return dir;
}

void RefreshWrapperRomList() {
	wrapperRomList.clear();
	std::filesystem::path romsDir = GetExecutableDir() / "roms";
	if(!std::filesystem::is_directory(romsDir)) {
		printf("ROMs folder not found: %s\n", romsDir.c_str());
		return;
	}
	for(const auto& entry : std::filesystem::directory_iterator(romsDir)) {
		if(!entry.is_regular_file()) continue;
		std::string ext = entry.path().extension().string();
		for(char& c : ext) c = (char)tolower((unsigned char)c);
		if(ext == ".gtr") {
			wrapperRomList.push_back(entry.path());
		}
	}
	std::sort(wrapperRomList.begin(), wrapperRomList.end(),
		[](const std::filesystem::path& a, const std::filesystem::path& b) {
			return a.filename().string() < b.filename().string();
		});
	if(wrapperRomListSelected >= (int)wrapperRomList.size()) {
		wrapperRomListSelected = 0;
	}
	printf("Found %zu ROM(s) in %s\n", wrapperRomList.size(), romsDir.c_str());
}

bool OpenWrapperRom(const std::filesystem::path& path) {
	const std::string pathStr = path.string();
	printf("Opening ROM: %s\n", pathStr.c_str());

	// Pause while swapping so the old game does not keep running mid-load.
	// Do not wipe the cartridge until the new file is confirmed loaded.
	PauseEmulation();
	if(joysticks) {
		joysticks->SetHeldButtons(0);
		joysticks->Reset();
	}
	cartridge_state.write_mode = false;

	if(LoadRomFile(pathStr.c_str()) != 0) {
		printf("Failed to load ROM: %s\n", pathStr.c_str());
		// Keep whatever was already in memory and unstick pause so the UI is usable
		ResumeEmulation();
		return false;
	}

	// Bytes past the new ROM size must not keep data from a previous larger cart
	if(cartridge_state.rom && cartridge_state.size > 0 && cartridge_state.size < (1 << 21)) {
		memset(cartridge_state.rom + cartridge_state.size, 0xFF, (size_t)((1 << 21) - cartridge_state.size));
	}

	romLoaded = true;
	showMenu = false;
	SyncWrapperMenuInput();
	cpu_core->Reset();
	cartridge_state.write_mode = false;
	if(joysticks) {
		joysticks->Reset();
	}
	ResumeEmulation();
	return true;
}

void QueueWrapperRomOpen(const std::filesystem::path& path) {
	wrapperPendingRom = path;
	wrapperRomLoadQueued = true;
	showMenu = false;
	SyncWrapperMenuInput();
}

void ProcessQueuedWrapperRomOpen() {
	if(!wrapperRomLoadQueued) return;
	wrapperRomLoadQueued = false;
	const std::filesystem::path path = wrapperPendingRom;
	wrapperPendingRom.clear();
	if(!OpenWrapperRom(path)) {
		showMenu = true;
		menuOpening = true;
		wrapperForceTab = TAB_GAMES;
	}
}

SDL_Texture* LoadWrapperPngTextureFromMemory(const unsigned char* data, unsigned int len) {
	int width = 0, height = 0, channels = 0;
	unsigned char* imgData = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 4);
	if(!imgData) {
		printf("Failed to decode embedded icon\n");
		return nullptr;
	}
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
		imgData, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
	SDL_Texture* tex = nullptr;
	if(surface) {
		tex = SDL_CreateTextureFromSurface(mainRenderer, surface);
		SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
		SDL_FreeSurface(surface);
	}
	stbi_image_free(imgData);
	return tex;
}

void LoadWrapperTabIcons() {
	if(!wrapperTabIconList) {
		wrapperTabIconList = LoadWrapperPngTextureFromMemory(img_list_icon_png, img_list_icon_png_len);
	}
	if(!wrapperTabIconSettings) {
		wrapperTabIconSettings = LoadWrapperPngTextureFromMemory(img_settings_icon_png, img_settings_icon_png_len);
	}
	if(!wrapperTabIconController) {
		wrapperTabIconController = LoadWrapperPngTextureFromMemory(img_controller_icon_png, img_controller_icon_png_len);
	}
}

void CancelWrapperRemap() {
	wrapperRemapListening = false;
	wrapperRemapTarget = -1;
	wrapperRemapConflict = false;
}

const char* GtButtonLabel(int buttonId) {
	switch(buttonId) {
		case GameTankButtons::P1_UP: return "Up";
		case GameTankButtons::P1_DOWN: return "Down";
		case GameTankButtons::P1_LEFT: return "Left";
		case GameTankButtons::P1_RIGHT: return "Right";
		case GameTankButtons::P1_A: return "A";
		case GameTankButtons::P1_B: return "B";
		case GameTankButtons::P1_C: return "C";
		case GameTankButtons::P1_START: return "Start";
		default: return "?";
	}
}

const char* JoyButtonLabel(uint8_t button) {
	switch(button) {
		case SDL_CONTROLLER_BUTTON_A: return "A";
		case SDL_CONTROLLER_BUTTON_B: return "B";
		case SDL_CONTROLLER_BUTTON_X: return "X";
		case SDL_CONTROLLER_BUTTON_Y: return "Y";
		case SDL_CONTROLLER_BUTTON_BACK: return "Select";
		case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
		case SDL_CONTROLLER_BUTTON_START: return "Start";
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Up";
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Down";
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Left";
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Right";
		default: return "Btn";
	}
}

const char* JoyAxisLabel(uint8_t axis) {
	switch(axis) {
		case SDL_CONTROLLER_AXIS_LEFTX: return "LX";
		case SDL_CONTROLLER_AXIS_LEFTY: return "LY";
		case SDL_CONTROLLER_AXIS_RIGHTX: return "RX";
		case SDL_CONTROLLER_AXIS_RIGHTY: return "RY";
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "LT";
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
		default: return "Axis";
	}
}

std::string FormatInputBinding(const InputBinding& bind) {
	char buf[48];
	switch(bind.type) {
		case BindingTypes::KEYBOARD:
			snprintf(buf, sizeof(buf), "Key %s", SDL_GetKeyName(bind.host_input.key));
			return buf;
		case BindingTypes::JOYSTICK_BUTTON:
		case BindingTypes::JOYSTICK_BUTTON_SYSTEM:
			return JoyButtonLabel(bind.host_input.joy_button);
		case BindingTypes::JOYSTICK_AXIS:
			snprintf(buf, sizeof(buf), "%s%s", JoyAxisLabel(bind.host_input.axis.axis),
				bind.host_input.axis.negative ? "-" : "+");
			return buf;
		case BindingTypes::JOYSTICK_HAT:
			return "Hat";
		default:
			return "?";
	}
}

int BindingRank(BindingTypes::BindingType type) {
	switch(type) {
		case BindingTypes::JOYSTICK_BUTTON:
		case BindingTypes::JOYSTICK_BUTTON_SYSTEM:
			return 3;
		case BindingTypes::JOYSTICK_AXIS:
			return 2;
		case BindingTypes::JOYSTICK_HAT:
			return 1;
		case BindingTypes::KEYBOARD:
			return 0;
		default:
			return -1;
	}
}

// Prefer joystick binding text for display; falls back to keyboard.
std::string BindingTextForButton(GameTankButtons::ButtonId buttonId) {
	int bestIdx = -1;
	int bestRank = -1;
	for(int i = 0; i < (int)joysticks->bindings.size(); ++i) {
		const InputBinding& b = joysticks->bindings[i];
		if(b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM) continue;
		if(b.button != buttonId) continue;
		const int rank = BindingRank(b.type);
		if(rank > bestRank) {
			bestRank = rank;
			bestIdx = i;
		}
	}
	if(bestIdx < 0) return "-";
	return FormatInputBinding(joysticks->bindings[bestIdx]);
}

std::string BindingTextForSystem() {
	for(const InputBinding& b : joysticks->bindings) {
		if(b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM) {
			return FormatInputBinding(b);
		}
	}
	return "-";
}

void ClearBindingsForButton(GameTankButtons::ButtonId buttonId) {
	auto& binds = joysticks->bindings;
	binds.erase(std::remove_if(binds.begin(), binds.end(),
		[buttonId](const InputBinding& b) {
			if(b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM) return false;
			return b.button == buttonId;
		}), binds.end());
	joysticks->SaveBindings();
}

void ClearSystemBinding() {
	auto& binds = joysticks->bindings;
	binds.erase(std::remove_if(binds.begin(), binds.end(),
		[](const InputBinding& b) {
			return b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM;
		}), binds.end());
	joysticks->SaveBindings();
}

bool HostInputsMatch(const InputBinding& a, const InputBinding& b) {
	const bool aPadBtn =
		a.type == BindingTypes::JOYSTICK_BUTTON ||
		a.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM;
	const bool bPadBtn =
		b.type == BindingTypes::JOYSTICK_BUTTON ||
		b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM;
	if(aPadBtn && bPadBtn) {
		return a.host_input.joy_button == b.host_input.joy_button;
	}
	if(a.type != b.type) {
		return false;
	}
	switch(a.type) {
		case BindingTypes::KEYBOARD:
			return a.host_input.key == b.host_input.key;
		case BindingTypes::JOYSTICK_AXIS:
			return a.host_input.axis.axis == b.host_input.axis.axis
				&& a.host_input.axis.negative == b.host_input.axis.negative;
		case BindingTypes::JOYSTICK_HAT:
			return a.host_input.joy_button == b.host_input.joy_button;
		default:
			return false;
	}
}

// True if this host input is already bound to a different GameTank action.
bool IsHostInputAlreadyMapped(const InputBinding& incoming) {
	for(const InputBinding& b : joysticks->bindings) {
		if(wrapperRemapTarget == -2) {
			if(b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM) continue;
		} else if(wrapperRemapTarget >= 0) {
			if(b.type != BindingTypes::JOYSTICK_BUTTON_SYSTEM
				&& b.button == (GameTankButtons::ButtonId)wrapperRemapTarget) {
				continue;
			}
		}
		if(HostInputsMatch(incoming, b)) {
			return true;
		}
	}
	return false;
}

void ApplyCapturedBinding(const InputBinding& incoming) {
	if(wrapperRemapTarget == -2) {
		ClearSystemBinding();
		InputBinding b = incoming;
		b.type = BindingTypes::JOYSTICK_BUTTON_SYSTEM;
		b.button = GameTankButtons::NO_BUTTON;
		joysticks->bindings.push_back(b);
	} else if(wrapperRemapTarget >= 0) {
		const auto buttonId = (GameTankButtons::ButtonId)wrapperRemapTarget;
		// Replace host bindings of the same class; keep other classes (e.g. keep keys when setting pad)
		auto& binds = joysticks->bindings;
		binds.erase(std::remove_if(binds.begin(), binds.end(),
			[&](const InputBinding& b) {
				if(b.type == BindingTypes::JOYSTICK_BUTTON_SYSTEM) return false;
				if(b.button != buttonId) return false;
				if(incoming.type == BindingTypes::KEYBOARD) {
					return b.type == BindingTypes::KEYBOARD;
				}
				return b.type == BindingTypes::JOYSTICK_BUTTON
					|| b.type == BindingTypes::JOYSTICK_AXIS
					|| b.type == BindingTypes::JOYSTICK_HAT;
			}), binds.end());
		InputBinding b = incoming;
		b.button = buttonId;
		joysticks->bindings.push_back(b);
	}
	joysticks->SaveBindings();
	CancelWrapperRemap();
}

bool TryCaptureWrapperRemap(const SDL_Event& e) {
	if(!wrapperRemapListening) return false;

	InputBinding captured{};
	bool got = false;

	if(e.type == SDL_KEYDOWN && e.key.repeat == 0) {
		if(e.key.keysym.sym == SDLK_ESCAPE) {
			CancelWrapperRemap();
			wrapperRemapConflict = false;
			return true;
		}
		captured.type = BindingTypes::KEYBOARD;
		captured.host_input.key = e.key.keysym.sym;
		got = true;
	} else if(e.type == SDL_CONTROLLERBUTTONDOWN) {
		// Select cancels remap unless we're binding the Menu action itself
		if(e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && wrapperRemapTarget != -2) {
			CancelWrapperRemap();
			wrapperRemapConflict = false;
			SyncWrapperMenuInput();
			return true;
		}
		captured.type = BindingTypes::JOYSTICK_BUTTON;
		captured.host_input.joy_button = e.cbutton.button;
		got = true;
	} else if(e.type == SDL_CONTROLLERAXISMOTION) {
		if(e.caxis.value > 16384 || e.caxis.value < -16384) {
			captured.type = BindingTypes::JOYSTICK_AXIS;
			captured.host_input.axis.axis = e.caxis.axis;
			captured.host_input.axis.negative = e.caxis.value < 0;
			got = true;
		}
	}

	if(got) {
		// Select/Back is reserved for the menu toggle
		if(captured.type == BindingTypes::JOYSTICK_BUTTON
			&& captured.host_input.joy_button == SDL_CONTROLLER_BUTTON_BACK) {
			wrapperRemapConflict = true;
			return true;
		}
		if(IsHostInputAlreadyMapped(captured)) {
			wrapperRemapConflict = true;
			return true; // consume input, keep listening
		}
		wrapperRemapConflict = false;
		ApplyCapturedBinding(captured);
		SyncWrapperMenuInput();
		return true;
	}
	return false;
}

void ResetWrapperControllerBindings() {
	joysticks->bindings.clear();
	load_joystick_defaults(joysticks->bindings);
	joysticks->SaveBindings();
	joysticks->Reset();
	CancelWrapperRemap();
}

const char* WrapperControllerName() {
	SDL_GameController* gc = joysticks ? joysticks->GetGameController() : nullptr;
	if(!gc) return "(no controller)";
	const char* name = SDL_GameControllerName(gc);
	return name ? name : "(controller)";
}
#endif

void randomize_memory() {
	for(int i = 0; i < RAMSIZE; i++) {
		system_state.ram[i] = rand() % 256;
		system_state.ram_initialized[i] = false;
	}

	for(int i = 0; i < VRAM_BUFFER_SIZE; i++) {
		system_state.vram[i] = rand() % 256;	
	}

	for(int i = 0; i < GRAM_BUFFER_SIZE; i++) {
		system_state.gram[i] = rand() % 256;	
	}
	
	system_state.dma_control = rand() % 256;
	system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
	system_state.banking = rand() % 256;
	blitter->gram_mid_bits = rand() % 4;
}

extern "C" {
void PauseEmulation() {
  paused = true;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = true;
}

void ResumeEmulation() {
  paused = false;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = false;
}
}

void CPUStopped() {
	PauseEmulation();
	printf("CPU stopped");
#ifdef TINYFILEDIALOGS_H
	tinyfd_notifyPopup("Alert",
		"CPU has stopped either due to STP opcode",
		"info");
#endif
}

const char * open_rom_dialog() {
	char const * lFilterPatterns[1] = {"*.gtr"};
#ifdef TINYFILEDIALOGS_H
	return tinyfd_openFileDialog(
		"Select a GameTank ROM file",
		"",
		1,
		lFilterPatterns,
		"GameTank Rom",
		0);
#else
	return EMBED_ROM_FILE;
#endif
}

extern "C" {
	// Attempts to load a rom by filename into a buffer
	// 0 on success
	// -1 on failure (e.g. file by name doesn't exist)
	int LoadRomFile(const char* filename) {
		std::filesystem::path filepath(filename);
		currentRomFilePath = filepath.string();
#ifdef WASM_BUILD
		std::filesystem::path nvramPath("/idbfs");
		nvramPath /= std::filesystem::path(currentRomFilePath).filename();
#else
		std::filesystem::path nvramPath(filename);
#endif
		nvramPath.replace_extension("sav");
		nvramFileFullPath = nvramPath.string();
		if (EmulatorConfig::xorFile != NULL) {
		  flashFileFullPath = std::string(EmulatorConfig::xorFile);
		} else {
		    nvramPath.replace_extension("xor");
		    flashFileFullPath = nvramPath.string();
		}
		nvramPath.replace_extension("gtrcfg");

		gameconfig = new GameConfig(nvramPath.string().c_str());

		std::filesystem::path defaultMemMapFilePath = filepath.parent_path().append("../build/out.map");
		std::filesystem::path defaultSourceMapFilePath = filepath.parent_path().append("../build/sourcemap.dbg");

		if(std::filesystem::exists(defaultMemMapFilePath)) {
			printf("found default memory map file location %s\n", defaultMemMapFilePath.c_str());
			loadedMemoryMap = new MemoryMap(defaultMemMapFilePath.string());
			Breakpoints::linkBreakpoints(*loadedMemoryMap);
		} else {
			loadedMemoryMap = new MemoryMap();
			printf("default memory map file %s not found\n", defaultMemMapFilePath.c_str());
		}

		if(std::filesystem::exists(defaultSourceMapFilePath)) {
			printf("found default source map file location %s\n", defaultSourceMapFilePath.c_str());
			std::string sourceMapPathString = defaultSourceMapFilePath.string();
			SourceMap::singleton = new SourceMap(sourceMapPathString);
		} else {
			printf("default source map file %s not found\n", defaultSourceMapFilePath.c_str());
		}

		printf("loading %s\n", filename);
		FILE* romFileP = fopen(filename, "rb");
		if(!romFileP) {
			printf("Unable to open file: %s\n", filename);
			return -1;
		}

		fseek(romFileP, 0L, SEEK_END);
		long fileSize = ftell(romFileP);
		constexpr long kMaxRomBytes = 1L << 21; // 2MB cartridge buffer
		if(fileSize <= 0 || fileSize > kMaxRomBytes) {
			printf("Invalid ROM size: %ld bytes (max %ld)\n", fileSize, kMaxRomBytes);
			fclose(romFileP);
			return -1;
		}
		cartridge_state.size = (int)fileSize;
		cartridge_state.write_mode = false;
		rewind(romFileP);
		switch(cartridge_state.size) {
			case 8192:
			loadedRomType = RomType::EEPROM8K;
			printf("Detected 8K (EEPROM)\n");
			break;
			case 32768:
			loadedRomType = RomType::EEPROM32K;
			printf("Detected 32K (EEPROM)\n");
			break;
			case 2097152:
			loadedRomType = RomType::FLASH2M;
			printf("Detected 2M (Flash)\n");
			break;
			default:
			loadedRomType = RomType::UNKNOWN;
			printf("Unknown ROM type: Size is %d bytes\n", cartridge_state.size);
			break;
		}
		const size_t got = fread(cartridge_state.rom, sizeof(uint8_t), (size_t)cartridge_state.size, romFileP);
		fclose(romFileP);
		if(got != (size_t)cartridge_state.size) {
			printf("Short ROM read: got %zu of %d bytes\n", got, cartridge_state.size);
			return -1;
		}
		if(cpu_core) {
			ResumeEmulation();
			cpu_core->Reset();
			cartridge_state.write_mode = false;
		}

		if(loadedRomType == RomType::FLASH2M) {

			if(std::filesystem::exists(flashFileFullPath.c_str())) {
				std::cout << "Loading flash save from " << flashFileFullPath << "\n";
				LoadModifiedFlash();
			} else {
				std::cout << "Couldn't find " << flashFileFullPath << "\n";
			}

			if(
				(cartridge_state.rom[0x1FFFF0] == 'S') &&
				(cartridge_state.rom[0x1FFFF1] == 'A') &&
				(cartridge_state.rom[0x1FFFF2] == 'V') &&
				(cartridge_state.rom[0x1FFFF3] == 'E')) {
					loadedRomType = RomType::FLASH2M_RAM32K;
					if(std::filesystem::exists(nvramFileFullPath.c_str())) {
						LoadNVRAM();
					}
				}
		}
		return 0;
	}

	void SetButtons(int buttonMask) {
		if(joysticks != NULL) {
			joysticks->SetHeldButtons(buttonMask);
		}
	}

	void takeScreenShot() {
		SDL_Surface *screenshot = SDL_CreateRGBSurface(0, viewport_width(display_scale), viewport_height(display_scale), 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
		SDL_RenderReadPixels(mainRenderer, NULL, SDL_PIXELFORMAT_ARGB8888, screenshot->pixels, screenshot->pitch);
		SDL_SaveBMP(screenshot, "screenshot.bmp");
		SDL_FreeSurface(screenshot);
	}

#ifdef WASM_BUILD
	extern "C" {
		EMSCRIPTEN_KEEPALIVE
		void SetPaddleMode(bool enabled) {
			paddle_emulation_enabled = enabled;
			if (paddle_emulation_enabled){
				if (paddle_touch_mode) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				}
				else
				{
					SDL_SetRelativeMouseMode(SDL_TRUE);
				}
			}
		}
		void SetPaddleTouchMode(bool enabled) {
			paddle_touch_mode = enabled;
			if (paddle_touch_mode) {
				SDL_SetRelativeMouseMode(SDL_FALSE);
			}
			else
			{
				SDL_SetRelativeMouseMode(SDL_TRUE);
			}
		}
		void EMSCRIPTEN_KEEPALIVE SetPaddleValue(int val) {
			if (joysticks != nullptr) {
				joysticks->SetPaddleBitsDirect(val); 
			}
    	}

		void EMSCRIPTEN_KEEPALIVE UpdatePaddleFromMouseJS(int index, int dx) {
			joysticks->UpdatePaddleFromMouse(0, dx);
		}

	}
	#endif
}
#ifndef WASM_BUILD
template <typename T>
void closeToolByType() {
    toolWindows.erase(
        std::remove_if(
            toolWindows.begin(),
            toolWindows.end(),
            [](BaseWindow* window) {
                if(dynamic_cast<T*>(window) != nullptr) {
					delete window;
					return true;
				}
				return false;
            }
        ),
        toolWindows.end()
    );
}

template <typename T>
bool toolTypeIsOpen() {
    for (const auto& window : toolWindows) {
        if (dynamic_cast<T*>(window) != nullptr) {
            return true;
        }
    }
    return false;
}

void toggleProfilerWindow() {
	if(!toolTypeIsOpen<ProfilerWindow>()) {
		toolWindows.push_back(new ProfilerWindow(profiler));
	} else {
		closeToolByType<ProfilerWindow>();
	}
}

void toggleMemBrowserWindow() {
	if(!toolTypeIsOpen<MemBrowserWindow>()) {
		toolWindows.push_back(new MemBrowserWindow(loadedMemoryMap, MemoryReadResolve, GetRAM, *gameconfig));
	} else {
		closeToolByType<MemBrowserWindow>();
	}
}

void toggleVRAMWindow() {
	if(!toolTypeIsOpen<VRAMWindow>()) {
		toolWindows.push_back(new VRAMWindow(vRAM_Surface, gRAM_Surface,
			&system_state, cpu_core, &cartridge_state));
	} else {
		closeToolByType<VRAMWindow>();
	}
}

void toggleSteppingWindow() {
	if(!toolTypeIsOpen<SteppingWindow>()) {
		toolWindows.push_back(new SteppingWindow(timekeeper, loadedMemoryMap, cpu_core, *gameconfig, cartridge_state));
	} else {
		closeToolByType<SteppingWindow>();
	}
}

void togglePatchingWindow() {
	if(!toolTypeIsOpen<PatchingWindow>()) {
		toolWindows.push_back(new PatchingWindow(loadedMemoryMap, gameconfig));
	} else {
		closeToolByType<PatchingWindow>();
	}
}

void doRamDump() {
	soundcard->dump_ram("audio_debug.dat");
	ofstream dumpfile ("ram_debug.dat", ios::out | ios::binary);
	dumpfile.write((char*) system_state.ram, RAMSIZE);
	dumpfile.close();
}

void toggleControllerOptionsWindow() {
	if(!toolTypeIsOpen<ControllerOptionsWindow>()) {
		toolWindows.push_back(new ControllerOptionsWindow(joysticks));
	} else {
		closeToolByType<ControllerOptionsWindow>();
	}
}

#endif

void toggleFullScreen() {
#if defined(WRAPPER_MODE) && !defined(CONSOLE_DISPLAY_FULLSCREEN)
	return;
#else
	if(isFullScreen) {
		SDL_SetWindowFullscreen(mainWindow, 0);
		isFullScreen = false;
	} else {
		SDL_SetWindowFullscreen(mainWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
		isFullScreen = true;
	}
	timekeeper.scaling_increment = INITIAL_SCALING_INCREMENT;
#endif
}

void toggleMute() {
	muteMask = muteMask ^ MUTE_SOURCE_MANUAL;
	ACPState* acp = AudioCoprocessor::singleton_acp_state;
	if(acp->device) SDL_LockAudioDevice(acp->device);
	acp->isMuted = (muteMask != 0);
	if(acp->device) SDL_UnlockAudioDevice(acp->device);
}

void setMenuMute(bool muted) {
	muteMask &= ~MUTE_SOURCE_MENU;
	if(muted) {
		muteMask |= MUTE_SOURCE_MENU;
	}
	ACPState* acp = AudioCoprocessor::singleton_acp_state;
	if(acp->device) SDL_LockAudioDevice(acp->device);
	acp->isMuted = (muteMask != 0);
	if(acp->device) SDL_UnlockAudioDevice(acp->device);
}

typedef struct HotkeyAssignment {
	void (*func)();
	SDL_Keycode  key;
} HotkeyAssignment;

HotkeyAssignment hotkeys[] = {
	{&toggleFullScreen, SDLK_F11},
	{&toggleMute, SDLK_m},
#if !defined(WASM_BUILD) && !defined(WRAPPER_MODE)
	{&doRamDump, SDLK_F6},
	{&toggleSteppingWindow, SDLK_F7},
	{&takeScreenShot, SDLK_F8},
	{&toggleMemBrowserWindow, SDLK_F9},
	{&toggleVRAMWindow, SDLK_F10},
	{&toggleProfilerWindow, SDLK_F12},
#endif
};

bool checkHotkey(SDL_Keycode  key) {
	for(HotkeyAssignment assignment : hotkeys) {
		if(assignment.key == key) {
			assignment.func();
			return true;
		}
	}
	return false;
}

#ifndef EM_BOOL
#define EM_BOOL int
#endif

void refreshScreen() {
	SDL_Rect src, dest, viewport_dest;
	int scr_w, scr_h;
	src.x = 0;
	src.y = (system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0;
	src.w = GT_WIDTH;
	src.h = GT_HEIGHT;
	SDL_GetWindowSize(mainWindow, &scr_w, &scr_h);
#ifdef WRAPPER_MODE
	// CRT/console: always use the user scale (1x or 2x), never downscale to fit.
	int scale = display_scale;
	if(scale < 1) scale = 1;
	if(scale > 2) scale = 2;
#else
	int scale_h = scr_h / GT_HEIGHT;
	int scale_w = (scr_w * VIEWPORT_ASPECT_H) / (GT_HEIGHT * VIEWPORT_ASPECT_W);
	int scale = min(scale_h, scale_w);
	if(scale < MIN_DISPLAY_SCALE) scale = MIN_DISPLAY_SCALE;
#endif
	viewport_dest.w = viewport_width(scale);
	viewport_dest.h = viewport_height(scale);
	viewport_dest.x = (scr_w - viewport_dest.w) / 2;
	viewport_dest.y = (scr_h - viewport_dest.h) / 2;
#ifdef WRAPPER_MODE
	viewport_dest.x += imageOffsetX;
	viewport_dest.y += imageOffsetY;
#endif
	dest.w = GT_WIDTH * scale;
	dest.h = GT_HEIGHT * scale;
	dest.x = viewport_dest.x + (viewport_dest.w - dest.w) / 2;
	dest.y = viewport_dest.y;
	SDL_UpdateTexture(framebufferTexture, NULL, vRAM_Surface->pixels, vRAM_Surface->pitch);

	SDL_RenderClear(mainRenderer);
	SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

	if(grid_mode != GRID_NONE && scale >= 1) {
		static const Uint8 grid_alphas[] = { 0, 64, 128, 255 };
		rebuildGridOverlay(scale);
		SDL_SetTextureAlphaMod(gridOverlayTexture, grid_alphas[grid_mode]);
		SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, NULL, &dest);
	}
	
	src.x = GT_WIDTH-1;
	src.w = 1;
	dest.w = dest.w * 86.0 / 512.0;
	dest.x -= dest.w;

	SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

	dest.x += dest.w + dest.h;

	SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

	if(grid_mode != GRID_NONE && scale >= 1) {
		static const Uint8 grid_alphas[] = { 0, 64, 128, 255 };
		src.x = 0;
		src.y = 0;
		src.w = dest.w;
		src.h = dest.h;
		SDL_SetTextureAlphaMod(gridOverlayTexture, grid_alphas[grid_mode]);
		SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, &src, &dest);
		dest.x -= dest.w + dest.h;
		src.x += scale - (dest.w % scale);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, &src, &dest);
	}

#if !defined(WASM_BUILD)
	ImGui::SetCurrentContext(main_imgui_ctx);
#ifdef WRAPPER_MODE
	{
		// Menu owns all directional input; suppress ImGui nav so tabs/sliders don't fight us.
		ImGuiIO& io_nav = ImGui::GetIO();
		if(showMenu) {
			io_nav.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
		} else {
			io_nav.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		}
	}
#endif
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	if(showMenu) {
#ifndef WRAPPER_MODE
		if(ImGui::BeginMainMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if(ImGui::MenuItem("Open Rom")) {
					const char* rom_file_name = open_rom_dialog();
					if(rom_file_name) {
						LoadRomFile(rom_file_name);
					}	
				}
				if(ImGui::MenuItem("Exit")) {
					running = false;
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu("Settings")) {
				if(ImGui::MenuItem("Controllers")) {
					toggleControllerOptionsWindow();
				}
				ImGui::MenuItem("Toggle Instant Blits", NULL, &(blitter->instant_mode));
				ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256);
				ImGui::Checkbox("Mute", &AudioCoprocessor::singleton_acp_state->isMuted);
				if (ImGui::Checkbox("Enable Paddle Emulation", &paddle_emulation_enabled)) {
					joysticks->SetHeldButtons(0);//clear bits on change just in case
				}
				if(ImGui::BeginMenu("Pallete")) {
					ImGui::RadioButton("Unscaled Capture", &palette_select, PALETTE_SELECT_CAPTURE);
					ImGui::RadioButton("Full Contrast", &palette_select, PALETTE_SELECT_SCALED);
					ImGui::RadioButton("Cheap HDMI converter", &palette_select, PALETTE_SELECT_HDMI);
					ImGui::RadioButton("Flawed Theory (Legacy)", &palette_select, PALETTE_SELECT_OLD);
					ImGui::EndMenu();
				}
				if(ImGui::BeginMenu("Scale")) {
					int prev_scale = display_scale;
					ImGui::RadioButton("2x", &display_scale, 2);
					ImGui::RadioButton("3x", &display_scale, 3);
					ImGui::RadioButton("4x", &display_scale, 4);
					ImGui::RadioButton("5x", &display_scale, 5);
					ImGui::RadioButton("6x", &display_scale, 6);
					if(display_scale != prev_scale && !isFullScreen) {
						SDL_SetWindowSize(mainWindow, viewport_width(display_scale), viewport_height(display_scale));
					}
					ImGui::EndMenu();
				}
				if(ImGui::BeginMenu("Grid")) {
					ImGui::RadioButton("No grid", &grid_mode, GRID_NONE);
					ImGui::RadioButton("25% opacity", &grid_mode, GRID_25);
					ImGui::RadioButton("50% opacity", &grid_mode, GRID_50);
					ImGui::RadioButton("Full black grid", &grid_mode, GRID_FULL);
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu("Tools")) {
				if(ImGui::MenuItem("Profiler (F12)")) {
					toggleProfilerWindow();
				}
				if(ImGui::MenuItem("Memory Browser (F9)")) {
					toggleMemBrowserWindow();
				}
				if(ImGui::MenuItem("VRAM Viewer (F10)")) {
					toggleVRAMWindow();
				}
				if(ImGui::MenuItem("Code Stepper (F7)")) {
					toggleSteppingWindow();
				}
				if(ImGui::MenuItem("Patching Window")) {
					togglePatchingWindow();
				}
				if(ImGui::MenuItem("Update Patches")) {
					gameconfig->UpdateAllPatches(cartridge_state.rom);
				}
				if(ImGui::MenuItem("Dump RAM to file (F6)")) {
					doRamDump();
				}
				if(ImGui::MenuItem("Deep Profile Single Vsync")) {
					vsyncProfileArmed = true;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
#else
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f));
		ImGui::Begin("OverlayBackground", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus);
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		ImGuiIO& io = ImGui::GetIO();
		const float panel_w = (io.DisplaySize.x < 266.0f) ? io.DisplaySize.x - 16.0f : 250.0f;
		const float panel_h = (io.DisplaySize.y < 196.0f) ? io.DisplaySize.y - 16.0f : 180.0f;
		ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - panel_w) * 0.5f, (io.DisplaySize.y - panel_h) * 0.5f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(menuPanelAlpha);
		if(!ImGui::IsAnyItemFocused()) {
			ImGui::SetNextWindowFocus();
		}

		// Compact controls sized for 10px Proggy Tiny
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f);

		ImGui::Begin("MainPanel", nullptr,
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar);

		const bool panelJustOpened = ImGui::IsWindowAppearing() || menuOpening;
		if (panelJustOpened) {
			if(wrapperForceTab < 0) {
				wrapperForceTab = wrapperUiTab; // restore last tab
			}
			if(wrapperForceTab == TAB_GAMES) {
				RefreshWrapperRomList();
			}
			wrapperSettingsNav = 0;
			wrapperCtrlNav = 0;
			wrapperCtrlCol = 0;
			CancelWrapperRemap();
			SyncWrapperMenuInput();
		}
		menuOpening = false;

		static int lastSettingsScroll = -1;
		static int lastCtrlScroll = -1;
		if(panelJustOpened) {
			lastSettingsScroll = -1;
			lastCtrlScroll = -1;
		}

		enum {
			SET_QUIT = 0,
			SET_RESET_GAME,
			SET_OPACITY,
			SET_PAUSE,
			SET_SCALE_1X,
			SET_SCALE_2X,
			SET_OFF_X,
			SET_OFF_Y,
			SET_PAL_CAPTURE,
			SET_PAL_SCALED,
			SET_PAL_HDMI,
			SET_PAL_OLD,
			SET_VOLUME,
			SET_MUTE,
			SET_COUNT
		};

		enum {
			CTRL_UP = 0,
			CTRL_DOWN,
			CTRL_LEFT,
			CTRL_RIGHT,
			CTRL_A,
			CTRL_B,
			CTRL_C,
			CTRL_START,
			CTRL_RESET_ALL,
			CTRL_COUNT
		};

		// SDL edge-detect input (avoids ImGui missing A when it was held across menu open)
		const WrapperMenuInput menuIn = PollWrapperMenuInput();
		if(!wrapperRemapListening) {
			if(menuIn.tabLeft) {
				wrapperForceTab = (wrapperUiTab + kWrapperTabCount - 1) % kWrapperTabCount;
			}
			if(menuIn.tabRight) {
				wrapperForceTab = (wrapperUiTab + 1) % kWrapperTabCount;
			}
		}
		const bool onHSlider =
			(wrapperUiTab == TAB_OPTIONS) && (
				wrapperSettingsNav == SET_OPACITY ||
				wrapperSettingsNav == SET_OFF_X ||
				wrapperSettingsNav == SET_OFF_Y ||
				wrapperSettingsNav == SET_VOLUME);
		const bool onCtrlMapRow =
			(wrapperUiTab == TAB_CONTROLLER) && (wrapperCtrlNav < CTRL_RESET_ALL);
		// Shoulders / Tab (Shift+Tab) switch tabs. Arrow keys adjust sliders / Set·Clr only.
		const bool navUp = !wrapperRemapListening && menuIn.up;
		const bool navDown = !wrapperRemapListening && menuIn.down;
		const bool navActivate = !wrapperRemapListening && menuIn.activate;
		const bool navDec = !wrapperRemapListening && (menuIn.dec || (onHSlider && menuIn.left) || (onCtrlMapRow && menuIn.left));
		const bool navInc = !wrapperRemapListening && (menuIn.inc || (onHSlider && menuIn.right) || (onCtrlMapRow && menuIn.right));

		if(wrapperForceTab >= 0) {
			wrapperUiTab = wrapperForceTab;
			wrapperForceTab = -1;
			CancelWrapperRemap();
			if(wrapperUiTab == TAB_GAMES) {
				RefreshWrapperRomList();
			}
			lastSettingsScroll = -1;
			lastCtrlScroll = -1;
		}

		auto focusBegin = [](bool on) {
			if(on) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
			} else {
				// 60% transparent when not selected
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.4f));
			}
		};
		auto focusEnd = [](bool) {
			ImGui::PopStyleColor(1);
		};

		// Icon tab bar — rounded tops, active tab merges into panel (RGB-Pi style)
		{
			SDL_Texture* icons[kWrapperTabCount] = {
				wrapperTabIconList, wrapperTabIconSettings, wrapperTabIconController
			};
			constexpr float kTabW = 28.0f;
			constexpr float kTabH = 20.0f;
			constexpr float kTabGap = 3.0f;
			constexpr float kTabRound = 4.0f;
			const ImU32 idleCol = IM_COL32(45, 95, 175, 255);
			const ImU32 selCol = IM_COL32(70, 145, 230, 255);
			const ImU32 lineCol = IM_COL32(70, 145, 230, 255);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 rowStart = ImGui::GetCursorScreenPos();
			const float rowWidth = ImGui::GetContentRegionAvail().x;

			for(int i = 0; i < kWrapperTabCount; ++i) {
				if(i > 0) ImGui::SameLine(0.0f, kTabGap);
				ImGui::PushID(i);
				const bool sel = (wrapperUiTab == i);
				const ImVec2 p0 = ImGui::GetCursorScreenPos();
				const ImVec2 p1(p0.x + kTabW, p0.y + kTabH);
				dl->AddRectFilled(p0, p1, sel ? selCol : idleCol, kTabRound, ImDrawFlags_RoundCornersTop);
				if(icons[i]) {
					constexpr float kIcon = 16.0f;
					const float ix = p0.x + (kTabW - kIcon) * 0.5f;
					const float iy = p0.y + (kTabH - kIcon) * 0.5f;
					dl->AddImage(
						ImTextureRef((ImTextureID)(intptr_t)icons[i]),
						ImVec2(ix, iy),
						ImVec2(ix + kIcon, iy + kIcon),
						ImVec2(0, 0), ImVec2(1, 1),
						IM_COL32(255, 255, 255, 255));
				}
				if(ImGui::InvisibleButton("##tab", ImVec2(kTabW, kTabH))) {
					wrapperForceTab = i;
				}
				ImGui::PopID();
			}

			// Base line under tabs; active tab punches a gap so it merges with content
			const float lineY = rowStart.y + kTabH;
			float lineX = rowStart.x;
			const float lineEnd = rowStart.x + rowWidth;
			for(int i = 0; i < kWrapperTabCount; ++i) {
				const float tabX0 = rowStart.x + i * (kTabW + kTabGap);
				const float tabX1 = tabX0 + kTabW;
				if(i == wrapperUiTab) {
					if(lineX < tabX0) {
						dl->AddLine(ImVec2(lineX, lineY), ImVec2(tabX0, lineY), lineCol, 1.0f);
					}
					lineX = tabX1;
				}
			}
			if(lineX < lineEnd) {
				dl->AddLine(ImVec2(lineX, lineY), ImVec2(lineEnd, lineY), lineCol, 1.0f);
			}
			ImGui::SetCursorScreenPos(ImVec2(rowStart.x, lineY + 4.0f));
			ImGui::Dummy(ImVec2(rowWidth, 0.0f));

			if(wrapperForceTab >= 0) {
				wrapperUiTab = wrapperForceTab;
				wrapperForceTab = -1;
				CancelWrapperRemap();
				if(wrapperUiTab == TAB_GAMES) {
					RefreshWrapperRomList();
				}
				lastSettingsScroll = -1;
				lastCtrlScroll = -1;
			}
		}

		if (ImGui::BeginChild("PanelBody", ImVec2(0, 0), ImGuiChildFlags_None)) {
			if (wrapperUiTab == TAB_GAMES) {
					if(wrapperRomList.empty()) {
						focusBegin(false);
						ImGui::TextWrapped("No .gtr files in roms/");
						focusEnd(false);
						focusBegin(true);
						if(ImGui::Button("Refresh") || navActivate) {
							RefreshWrapperRomList();
						}
						focusEnd(true);
					} else {
						bool activateRom = false;
						if(navDown && wrapperRomListSelected + 1 < (int)wrapperRomList.size()) {
							++wrapperRomListSelected;
						}
						if(navUp && wrapperRomListSelected > 0) {
							--wrapperRomListSelected;
						}
						if(navActivate) {
							activateRom = true;
						}

						if(ImGui::BeginChild("RomList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
							static int lastScrolledRom = -1;
							ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
							for(int i = 0; i < (int)wrapperRomList.size(); ++i) {
								const bool selected = (i == wrapperRomListSelected);
								std::string label = wrapperRomList[i].stem().string();
								focusBegin(selected);
								if(ImGui::Selectable(label.c_str(), selected)) {
									wrapperRomListSelected = i;
									activateRom = true;
								}
								focusEnd(selected);
								if(selected && lastScrolledRom != wrapperRomListSelected) {
									ImGui::SetScrollHereY(0.5f);
									lastScrolledRom = wrapperRomListSelected;
								}
							}
							ImGui::PopItemFlag();
						}
						ImGui::EndChild();

						if(activateRom) {
							QueueWrapperRomOpen(wrapperRomList[wrapperRomListSelected]);
						}
					}
			} else if (wrapperUiTab == TAB_OPTIONS) {
					if(navDown && wrapperSettingsNav + 1 < SET_COUNT) {
						++wrapperSettingsNav;
					}
					if(navUp && wrapperSettingsNav > 0) {
						--wrapperSettingsNav;
					}

					auto scrollIfFocused = [&](bool on, int idx) {
						if(on && lastSettingsScroll != idx) {
							ImGui::SetScrollHereY(0.5f);
							lastSettingsScroll = idx;
						}
					};

					if(ImGui::BeginChild("SettingsScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
						ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

						bool f = (wrapperSettingsNav == SET_QUIT);
						focusBegin(f);
						if(ImGui::Button("Quit", ImVec2(40, 0)) || (f && navActivate)) {
							running = false;
						}
						focusEnd(f);
						scrollIfFocused(f, SET_QUIT);

						f = (wrapperSettingsNav == SET_RESET_GAME);
						focusBegin(f);
						if(ImGui::Button("Reset game") || (f && navActivate)) {
							if(romLoaded) {
								resetQueued = 1;
								showMenu = false;
								SyncWrapperMenuInput();
								joysticks->Reset();
							}
						}
						focusEnd(f);
						scrollIfFocused(f, SET_RESET_GAME);

						ImGui::Spacing();
						ImGui::Separator();
						focusBegin(false);
						ImGui::TextUnformatted("Settings");
						focusEnd(false);

						f = (wrapperSettingsNav == SET_OPACITY);
						focusBegin(f);
						ImGui::SliderFloat("Opacity", &menuPanelAlpha, 0.20f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput);
						if(ImGui::IsItemDeactivatedAfterEdit()) MarkWrapperSettingsDirty();
						focusEnd(f);
						scrollIfFocused(f, SET_OPACITY);
						if(f && navDec) { menuPanelAlpha = std::max(0.20f, menuPanelAlpha - 0.05f); MarkWrapperSettingsDirty(); }
						if(f && navInc) { menuPanelAlpha = std::min(1.0f, menuPanelAlpha + 0.05f); MarkWrapperSettingsDirty(); }

						f = (wrapperSettingsNav == SET_PAUSE);
						focusBegin(f);
						{
							bool pauseVal = pauseWhenMenuOpen;
							if(ImGui::Checkbox("Pause when open", &pauseVal)) {
								pauseWhenMenuOpen = pauseVal;
								MarkWrapperSettingsDirty();
							} else if(f && navActivate) {
								pauseWhenMenuOpen = !pauseWhenMenuOpen;
								MarkWrapperSettingsDirty();
							}
						}
						focusEnd(f);
						scrollIfFocused(f, SET_PAUSE);

						focusBegin(false);
						ImGui::TextUnformatted("Image size");
						focusEnd(false);
						f = (wrapperSettingsNav == SET_SCALE_1X);
						focusBegin(f);
						if(ImGui::RadioButton("1x", display_scale == 1) || (f && navActivate)) {
							display_scale = 1;
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_SCALE_1X);
						f = (wrapperSettingsNav == SET_SCALE_2X);
						focusBegin(f);
						if(ImGui::RadioButton("2x", display_scale == 2) || (f && navActivate)) {
							display_scale = 2;
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_SCALE_2X);

						f = (wrapperSettingsNav == SET_OFF_X);
						focusBegin(f);
						ImGui::SliderInt("Offset X", &imageOffsetX, -64, 64, "%d", ImGuiSliderFlags_NoInput);
						if(ImGui::IsItemDeactivatedAfterEdit()) MarkWrapperSettingsDirty();
						focusEnd(f);
						scrollIfFocused(f, SET_OFF_X);
						if(f && navDec) { imageOffsetX = std::max(-64, imageOffsetX - 1); MarkWrapperSettingsDirty(); }
						if(f && navInc) { imageOffsetX = std::min(64, imageOffsetX + 1); MarkWrapperSettingsDirty(); }

						f = (wrapperSettingsNav == SET_OFF_Y);
						focusBegin(f);
						ImGui::SliderInt("Offset Y", &imageOffsetY, -64, 64, "%d", ImGuiSliderFlags_NoInput);
						if(ImGui::IsItemDeactivatedAfterEdit()) MarkWrapperSettingsDirty();
						focusEnd(f);
						scrollIfFocused(f, SET_OFF_Y);
						if(f && navDec) { imageOffsetY = std::max(-64, imageOffsetY - 1); MarkWrapperSettingsDirty(); }
						if(f && navInc) { imageOffsetY = std::min(64, imageOffsetY + 1); MarkWrapperSettingsDirty(); }

						ImGui::Spacing();
						ImGui::Separator();
						focusBegin(false);
						ImGui::TextUnformatted("Palette");
						focusEnd(false);
						f = (wrapperSettingsNav == SET_PAL_CAPTURE);
						focusBegin(f);
						if(ImGui::RadioButton("Unscaled Capture", palette_select == PALETTE_SELECT_CAPTURE) || (f && navActivate)) {
							palette_select = PALETTE_SELECT_CAPTURE;
							RefreshPaletteBuffers();
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_PAL_CAPTURE);

						f = (wrapperSettingsNav == SET_PAL_SCALED);
						focusBegin(f);
						if(ImGui::RadioButton("Full Contrast", palette_select == PALETTE_SELECT_SCALED) || (f && navActivate)) {
							palette_select = PALETTE_SELECT_SCALED;
							RefreshPaletteBuffers();
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_PAL_SCALED);

						f = (wrapperSettingsNav == SET_PAL_HDMI);
						focusBegin(f);
						if(ImGui::RadioButton("Cheap HDMI", palette_select == PALETTE_SELECT_HDMI) || (f && navActivate)) {
							palette_select = PALETTE_SELECT_HDMI;
							RefreshPaletteBuffers();
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_PAL_HDMI);

						f = (wrapperSettingsNav == SET_PAL_OLD);
						focusBegin(f);
						if(ImGui::RadioButton("Legacy", palette_select == PALETTE_SELECT_OLD) || (f && navActivate)) {
							palette_select = PALETTE_SELECT_OLD;
							RefreshPaletteBuffers();
							MarkWrapperSettingsDirty();
						}
						focusEnd(f);
						scrollIfFocused(f, SET_PAL_OLD);

						ImGui::Spacing();
						ImGui::Separator();
						focusBegin(false);
						ImGui::TextUnformatted("Audio");
						focusEnd(false);
						f = (wrapperSettingsNav == SET_VOLUME);
						focusBegin(f);
						ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256, "%d", ImGuiSliderFlags_NoInput);
						if(ImGui::IsItemDeactivatedAfterEdit()) MarkWrapperSettingsDirty();
						focusEnd(f);
						scrollIfFocused(f, SET_VOLUME);
						if(f && navDec) {
							AudioCoprocessor::singleton_acp_state->volume = std::max(0, AudioCoprocessor::singleton_acp_state->volume - 8);
							MarkWrapperSettingsDirty();
						}
						if(f && navInc) {
							AudioCoprocessor::singleton_acp_state->volume = std::min(256, AudioCoprocessor::singleton_acp_state->volume + 8);
							MarkWrapperSettingsDirty();
						}

						f = (wrapperSettingsNav == SET_MUTE);
						focusBegin(f);
						{
							bool appMute = (muteMask & MUTE_SOURCE_MANUAL) != 0;
							bool changed = ImGui::Checkbox("Mute", &appMute);
							if(f && navActivate) {
								appMute = !appMute;
								changed = true;
							}
							if(changed) {
								if(appMute) muteMask |= MUTE_SOURCE_MANUAL;
								else muteMask &= ~MUTE_SOURCE_MANUAL;
								AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
								MarkWrapperSettingsDirty();
							}
						}
						focusEnd(f);
						scrollIfFocused(f, SET_MUTE);

						ImGui::PopItemFlag();
					}
					ImGui::EndChild();
			} else if (wrapperUiTab == TAB_CONTROLLER) {
					if(navDown && wrapperCtrlNav + 1 < CTRL_COUNT) {
						++wrapperCtrlNav;
					}
					if(navUp && wrapperCtrlNav > 0) {
						--wrapperCtrlNav;
					}
					if(onCtrlMapRow) {
						if(navDec) wrapperCtrlCol = 0;
						if(navInc) wrapperCtrlCol = 1;
					}

					focusBegin(false);
					ImGui::TextWrapped("%s", WrapperControllerName());
					focusEnd(false);
					ImGui::Separator();
					if(wrapperRemapListening) {
						focusBegin(true);
						ImGui::TextUnformatted(wrapperRemapConflict ? "Already mapped" : "Press a button...");
						focusEnd(true);
					}

					auto startRemap = [&](int target) {
						wrapperRemapListening = true;
						wrapperRemapTarget = target;
						wrapperRemapConflict = false;
						SyncWrapperMenuInput();
					};

					auto scrollCtrl = [&](bool on, int idx) {
						if(on && lastCtrlScroll != idx) {
							ImGui::SetScrollHereY(0.5f);
							lastCtrlScroll = idx;
						}
					};

					const float setW = 28.0f;
					const float clrW = 28.0f;
					const float rowBtnGap = 4.0f;

					if(ImGui::BeginChild("ControllerScroll", ImVec2(0, -28), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
						ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

						auto drawMapRow = [&](int navIdx, const char* label, const std::string& mapping, int remapTarget, auto onClear) {
							const bool rowFocus = (wrapperCtrlNav == navIdx);
							const bool listeningHere = wrapperRemapListening && (wrapperRemapTarget == remapTarget);
							const bool setFocus = rowFocus && wrapperCtrlCol == 0;
							const bool clrFocus = rowFocus && wrapperCtrlCol == 1;
							ImGui::PushID(navIdx);

							const float rowW = ImGui::GetContentRegionAvail().x;
							const float labelW = 42.0f;
							const float mapW = std::max(24.0f, rowW - labelW - setW - clrW - rowBtnGap * 2 - 4.0f);

							focusBegin(rowFocus);
							ImGui::TextUnformatted(label);
							focusEnd(rowFocus);
							ImGui::SameLine(labelW);
							focusBegin(rowFocus);
							if(listeningHere) {
								ImGui::TextUnformatted("...");
							} else {
								ImGui::TextUnformatted(mapping.c_str());
							}
							focusEnd(rowFocus);
							ImGui::SameLine(labelW + mapW);
							focusBegin(setFocus);
							if(ImGui::Button("Set", ImVec2(setW, 0)) || (setFocus && navActivate)) {
								startRemap(remapTarget);
							}
							focusEnd(setFocus);
							ImGui::SameLine(0.0f, rowBtnGap);
							focusBegin(clrFocus);
							if(ImGui::Button("Clr", ImVec2(clrW, 0)) || (clrFocus && navActivate)) {
								onClear();
								CancelWrapperRemap();
							}
							focusEnd(clrFocus);
							scrollCtrl(rowFocus, navIdx);
							ImGui::PopID();
						};

						drawMapRow(CTRL_UP, "Up", BindingTextForButton(GameTankButtons::P1_UP), GameTankButtons::P1_UP,
							[]{ ClearBindingsForButton(GameTankButtons::P1_UP); });
						drawMapRow(CTRL_DOWN, "Down", BindingTextForButton(GameTankButtons::P1_DOWN), GameTankButtons::P1_DOWN,
							[]{ ClearBindingsForButton(GameTankButtons::P1_DOWN); });
						drawMapRow(CTRL_LEFT, "Left", BindingTextForButton(GameTankButtons::P1_LEFT), GameTankButtons::P1_LEFT,
							[]{ ClearBindingsForButton(GameTankButtons::P1_LEFT); });
						drawMapRow(CTRL_RIGHT, "Right", BindingTextForButton(GameTankButtons::P1_RIGHT), GameTankButtons::P1_RIGHT,
							[]{ ClearBindingsForButton(GameTankButtons::P1_RIGHT); });
						drawMapRow(CTRL_A, "A", BindingTextForButton(GameTankButtons::P1_A), GameTankButtons::P1_A,
							[]{ ClearBindingsForButton(GameTankButtons::P1_A); });
						drawMapRow(CTRL_B, "B", BindingTextForButton(GameTankButtons::P1_B), GameTankButtons::P1_B,
							[]{ ClearBindingsForButton(GameTankButtons::P1_B); });
						drawMapRow(CTRL_C, "C", BindingTextForButton(GameTankButtons::P1_C), GameTankButtons::P1_C,
							[]{ ClearBindingsForButton(GameTankButtons::P1_C); });
						drawMapRow(CTRL_START, "Start", BindingTextForButton(GameTankButtons::P1_START), GameTankButtons::P1_START,
							[]{ ClearBindingsForButton(GameTankButtons::P1_START); });

						ImGui::PopItemFlag();
					}
					ImGui::EndChild();

					{
						const bool resetFocus = (wrapperCtrlNav == CTRL_RESET_ALL);
						focusBegin(resetFocus);
						if(ImGui::Button("Reset all") || (resetFocus && navActivate)) {
							ResetWrapperControllerBindings();
						}
						focusEnd(resetFocus);
						if(resetFocus && lastCtrlScroll != wrapperCtrlNav) {
							lastCtrlScroll = wrapperCtrlNav;
						}
					}
			}
		}
		ImGui::EndChild();

		ImGui::End();
		ImGui::PopStyleVar(6);
#endif
	}
	ImGui::Render();
	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), mainRenderer);
#endif
	SDL_RenderPresent(mainRenderer);
}

char titlebuf[256];
int32_t intended_cycles = 0;

#ifdef WASM_BUILD
double target_frame_period_ms = 1000.0 / 60.0;
double last_raf_time = 0;
double frame_time_accumulator = 0;
#endif

EM_BOOL mainloop(double time, void* userdata) {
#ifdef WASM_BUILD
        double delta_time = time - last_raf_time;
        frame_time_accumulator += delta_time;
        last_raf_time = time;
        if(frame_time_accumulator < target_frame_period_ms) {
                return true;
        }
        frame_time_accumulator -= target_frame_period_ms;
#else 
if (paddle_emulation_enabled) {
	if (paddle_touch_mode){ //touch / absolute
		int mx, my, winW, winH;
		SDL_GetMouseState(&mx, &my);
		SDL_GetWindowSize(mainWindow, &winW, &winH);
		joysticks->UpdatePaddleFromCursorPos(0, mx, winW);
	} else { //mouse mode delta
		if (showMenu) {
			if (SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
		} else {
			if (!SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_TRUE);
			int dx, dy;
			SDL_GetRelativeMouseState(&dx, &dy);
			joysticks->UpdatePaddleFromMouse(0, dx);
		}
	}
}//mouse paddle emulation
else {
    if(SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
}
	
#endif

#ifdef WRAPPER_MODE
	if(!paused && !(showMenu && pauseWhenMenuOpen)) {
#else
	if(!paused) {
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount;
#ifndef WASM_BUILD
			switch(timekeeper.clock_mode) {
				case CLOCKMODE_NORMAL:
					cpu_core->freeze = false;
					intended_cycles = timekeeper.cycles_per_vsync;
					break;
				case CLOCKMODE_SINGLE:
					cpu_core->freeze = false;
					Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, cpu_core->pc, 32);
					intended_cycles = 1;
					timekeeper.clock_mode = CLOCKMODE_STOPPED;
					break;
				case CLOCKMODE_STOPPED:
					intended_cycles = 0;
					break;
			}
			if(intended_cycles) {
				cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
			}
#else
			intended_cycles = timekeeper.cycles_per_vsync;
			cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount - timekeeper.actual_cycles;
			if(cpu_core->illegalOpcode) {
				printf("Hit illegal opcode %x\npc = %x\n", cpu_core->illegalOpcodeSrc, cpu_core->pc);
				PauseEmulation();
			} else if((timekeeper.clock_mode == CLOCKMODE_NORMAL) && (timekeeper.actual_cycles == 0)) {
				profiler.zeroConsec++;
				if(profiler.zeroConsec == 10) {
					printf("(Got stuck at 0x%x)\n", cpu_core->pc);
					PauseEmulation();
				}
				timekeeper.totalCyclesCount += intended_cycles;
			} else {
				profiler.zeroConsec = 0;
			}

#ifndef WASM_BUILD
			if(!gofast) {
				SDL_Delay(timekeeper.time_scaling * intended_cycles/timekeeper.system_clock);
			} else {
				timekeeper.lastTicks = 0;
			}
			timekeeper.currentTicks = SDL_GetTicks();

			if(timekeeper.clock_mode == CLOCKMODE_NORMAL) {
				if(timekeeper.lastTicks != 0) {
					int time_error = (timekeeper.currentTicks - timekeeper.lastTicks) - (1000 * intended_cycles/timekeeper.system_clock);
					if(timekeeper.frameCount == 100) {
#ifndef WRAPPER_MODE						
					  sprintf(titlebuf, "%s | %s | s: %.1f inc: %.1f err: %d\n", WINDOW_TITLE, currentRomFilePath.c_str(), timekeeper.time_scaling, timekeeper.scaling_increment, time_error);
						SDL_SetWindowTitle(mainWindow, titlebuf);
#endif
						profiler.fps = profiler.bufferFlipCount * 60 / 100;
						timekeeper.frameCount = 0;
						profiler.bufferFlipCount = 0;
					}
					bool overlong = time_error > 0;

					if(overlong == timekeeper.prev_overlong) {
						//scaling_increment = 1;
					} else if(timekeeper.scaling_increment > 1) {
						timekeeper.scaling_increment -= 1;
					}
					if((timekeeper.scaling_increment > 1) || (abs(time_error) > 2)) {
						if(overlong) {
							timekeeper.time_scaling -= timekeeper.scaling_increment;
						} else {
							timekeeper.time_scaling += timekeeper.scaling_increment;
						}
					}
					timekeeper.prev_overlong = overlong;

					if(timekeeper.time_scaling < 100) {
						timekeeper.time_scaling = 100;
					} else if(timekeeper.time_scaling > 2000) {
						timekeeper.time_scaling = 2000;
					}
				}
				timekeeper.lastTicks = timekeeper.currentTicks;
				timekeeper.frameCount++;
			}
#endif
			timekeeper.totalCyclesCount -= timekeeper.actual_cycles;
			timekeeper.totalCyclesCount += intended_cycles;
			timekeeper.cycles_since_vsync += intended_cycles;
			if(timekeeper.cycles_since_vsync >= timekeeper.cycles_per_vsync) {
				timekeeper.cycles_since_vsync -= timekeeper.cycles_per_vsync;
				if(system_state.dma_control & DMA_VSYNC_NMI_BIT) {
					cpu_core->NMI();
					if(vsyncProfileArmed) {
						profiler.DeepProfileStart();
						vsyncProfileArmed = false;
						vsyncProfileRunning = true;
					} else if(vsyncProfileRunning) {
						profiler.DeepProfileStop(loadedMemoryMap, SourceMap::singleton);
						vsyncProfileRunning = false;
					}
				}
				if(!profiler.measure_by_frameflip) {
					profiler.ResetTimers();
					profiler.last_blitter_activity = blitter->pixels_this_frame;
					blitter->pixels_this_frame = 0;
				}
			}
		} else {
				SDL_Delay(16);
		}
		blitter->CatchUp();
		

		if(EmulatorConfig::noSound) {
			AudioCoprocessor::fill_audio(AudioCoprocessor::singleton_acp_state, NULL, AudioCoprocessor::singleton_acp_state->samples_per_frame);
		}

		while( SDL_PollEvent( &e ) != 0 )
        {
#ifndef WASM_BUILD

#ifdef WRAPPER_MODE
			if(true){
#else
			if(SDL_GetMouseFocus() == mainWindow) {
#endif
				ImGui::SetCurrentContext(main_imgui_ctx);
				ImPlot::SetCurrentContext(main_implot_ctx);
				ImGui_ImplSDL2_ProcessEvent(&e);
			}
			for (auto toolWindow : toolWindows) {
				toolWindow->HandleEvent(e);
			}

			ImGui::SetCurrentContext(main_imgui_ctx);
			ImPlot::SetCurrentContext(main_implot_ctx);

#ifndef WRAPPER_MODE
			if(ImGui::GetIO().WantCaptureKeyboard && ((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP))) {
				continue;
			}
#endif //WRAPPER_MODE
#endif //WASM_BUILD
#ifdef WRAPPER_MODE
			if(TryCaptureWrapperRemap(e)) {
				continue;
			}
#endif
            //User requests quit
            if( e.type == SDL_QUIT )
            {
               running = false;
            } else if(e.type == SDL_WINDOWEVENT)
			{
				if(e.window.event == SDL_WINDOWEVENT_CLOSE) {
					SDL_Window* closedWindow = SDL_GetWindowFromID(e.window.windowID);
					if(closedWindow == mainWindow) {
						running = false;
					}
				}
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				} 
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
					if (paddle_emulation_enabled && !paddle_touch_mode) {
						SDL_SetRelativeMouseMode(SDL_TRUE);
					}
				}

			} else if((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP)) {
				if(e.key.repeat == 0) {
					if((e.type == SDL_KEYUP) || !checkHotkey(e.key.keysym.sym)) {
						switch(e.key.keysym.sym) {
							case SDLK_LSHIFT:
								lshift = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_RSHIFT:
								rshift = (e.type == SDL_KEYDOWN);
								break;							
							case SDLK_ESCAPE:
	#if !defined(DISABLE_ESC)
								if(e.type == SDL_KEYDOWN) {
#ifdef WRAPPER_MODE
									if(wrapperRemapListening) {
										CancelWrapperRemap();
										SyncWrapperMenuInput();
										break;
									}
#endif
									showMenu = !showMenu;
									menuOpening = showMenu;
#ifdef WRAPPER_MODE
									if(!showMenu) CancelWrapperRemap();
#endif
								}
								#endif
								break;
	#ifndef WRAPPER_MODE
							case SDLK_BACKQUOTE:
								gofast = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_r:
								//TODO add menu item for reset
								if(e.type == SDL_KEYDOWN) {
									if(lshift || rshift) {
										resetQueued = 2;
									} else {
										resetQueued = 1;
									}
								}
								break;
							case SDLK_o:
								if(e.type == SDL_KEYDOWN) {
									const char* rom_file_name = open_rom_dialog();
									if(rom_file_name) {
										LoadRomFile(rom_file_name);
									} else {
	#ifdef TINYFILEDIALOGS_H
										tinyfd_notifyPopup("Alert",
										"No ROM was loaded",
										"warning");
	#endif
									}
								}
								break;
	#endif
							default:
								joysticks->update(&e, showMenu || resetQueued);
								break;
						}
					}
				}
            } else {
				joysticks->update(&e, showMenu || resetQueued);
			}
        }

		if(joysticks->CheckSystemButtonPressed()) {
	#if !defined(DISABLE_ESC)
#ifdef WRAPPER_MODE
			if(wrapperRemapListening) {
				CancelWrapperRemap();
				SyncWrapperMenuInput();
			} else {
				showMenu = !showMenu;
				menuOpening = showMenu;
				if(!showMenu) CancelWrapperRemap();
			}
#else
			showMenu = !showMenu;
			menuOpening = showMenu;
#endif
	#endif
		}
#ifdef WRAPPER_MODE
		{
			static bool wasMenuOpen = false;
			if(wasMenuOpen && !showMenu) {
				SaveWrapperSettings();
			}
			wasMenuOpen = showMenu;
		}
		{
			const bool wantMenuMute = showMenu && pauseWhenMenuOpen;
			const bool haveMenuMute = (muteMask & MUTE_SOURCE_MENU) != 0;
			if(wantMenuMute != haveMenuMute) {
				setMenuMute(wantMenuMute);
			}
		}
		{
			int paletteDir = joysticks->CheckPaletteCycle();
			if(paletteDir != 0 && !showMenu) {
				CycleWrapperPalette(paletteDir);
			}
		}
#endif

		refreshScreen();
#ifdef WRAPPER_MODE
		ProcessQueuedWrapperRomOpen();
#endif
		SDL_UpdateWindowSurface(mainWindow);

#ifndef WASM_BUILD
		for (auto& window : toolWindows) {
			if(window->IsOpen()) {
				window->Draw();
			}
		}

		auto const to_be_removed = std::partition(begin(toolWindows), end(toolWindows), [](auto w){ return w->IsOpen(); });
		std::for_each(to_be_removed, end(toolWindows), [](auto w) {
			delete w;
		});
		toolWindows.erase(to_be_removed, end(toolWindows));
#endif
		
	if(!running) {
#ifdef WRAPPER_MODE
		SaveWrapperSettings();
#endif
#ifdef WASM_BUILD
		emscripten_cancel_main_loop();
#else
		for (auto& window : toolWindows) {
			delete window;
		}
		toolWindows.clear();

		ImGui::SetCurrentContext(main_imgui_ctx);
		ImPlot::SetCurrentContext(main_implot_ctx);
		ImPlot::DestroyContext(main_implot_ctx);
		ImGui_ImplSDLRenderer2_Shutdown();
    	ImGui_ImplSDL2_Shutdown();
    	ImGui::DestroyContext(main_imgui_ctx);
#endif
    	SDL_DestroyRenderer(mainRenderer);
		SDL_DestroyWindow(mainWindow);
	}

	if(resetQueued) {
		ResumeEmulation();
		if(lshift || rshift || (resetQueued == 2)) {
			randomize_memory();
			randomize_vram();
		}
		cpu_core->Reset();
		cartridge_state.write_mode = false;
		joysticks->SetHeldButtons(0);//clear paddle bits before reset
		joysticks->Reset();
		resetQueued = 0;
	}
	return running;
}

int main(int argC, char* argV[]) {
	srand(time(NULL));
	cartridge_state.rom = new uint8_t[1 << 21];

	const char* rom_file_name = NULL;

#ifdef EMBED_ROM_FILE
	rom_file_name = EMBED_ROM_FILE;
#else
	for(int argIdx = 1; argIdx < argC; ++argIdx) {
		if((argV[argIdx])[0] == '-') {
			EmulatorConfig::parseArg(argV[argIdx]);
		} else if(!rom_file_name) {
			rom_file_name = argV[argIdx];
		}
	}
#endif

#ifdef DEFAULT_ROM_PATH
	if(argC == 1) {
		int execPathLength = wai_getExecutablePath(NULL, 0, NULL);
		if(execPathLength != -1) {
			char* path = (char*)malloc(execPathLength + 1);
			wai_getExecutablePath(path, execPathLength, NULL);
			path[execPathLength] = '\0';
			std::filesystem::path execPath(path);
			free(path);
			std::filesystem::path romPath = execPath.parent_path() / DEFAULT_ROM_PATH;
			std::string romPathStr = (execPath.parent_path() / DEFAULT_ROM_PATH).string();
			rom_file_name = strdup(romPathStr.c_str());
		}
	}
#endif

	//cartridge_state.rom = new uint8_t [cartridge_state.size];
		for(int i = 0; i < cartridge_state.size; i++) {
			cartridge_state.rom[i] = 0;
		}

	
	soundcard = new AudioCoprocessor();
	cpu_core = new mos6502(MemoryRead, MemoryWrite, CPUStopped, MemorySync);
	cpu_core->Reset();
	cartridge_state.write_mode = false;
	blitter = new Blitter(cpu_core, &timekeeper, &system_state, vRAM_Surface);
	randomize_memory();
	
	SDL_Init(SDL_INIT_VIDEO);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	atexit(SDL_Quit);

	bmpFont = SDL_CreateRGBSurfaceFrom(font_map, 128, 128, 32, 4 * 128, rmask, gmask, bmask, amask);

	vRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 2, 32, rmask, gmask, bmask, amask);
	gRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 32, 32, rmask, gmask, bmask, amask);

	SDL_SetColorKey(vRAM_Surface, SDL_FALSE, 0);
	SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);

#if defined(WRAPPER_MODE) && defined(CONSOLE_DISPLAY_CRT)
	const int console_w = 342;
	const int console_h = 256;
	mainWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, console_w, console_h, SDL_WINDOW_SHOWN);
	SDL_SetWindowMinimumSize(mainWindow, console_w, console_h);
	SDL_SetWindowMaximumSize(mainWindow, console_w, console_h);
#elif defined(WRAPPER_MODE) && defined(CONSOLE_DISPLAY_FULLSCREEN)
	mainWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, viewport_width(display_scale), viewport_height(display_scale), SDL_WINDOW_SHOWN);
#else
	mainWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, viewport_width(display_scale), viewport_height(display_scale), SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	SDL_SetWindowMinimumSize(mainWindow, viewport_width(MIN_DISPLAY_SCALE), viewport_height(MIN_DISPLAY_SCALE));
#endif
	mainRenderer = SDL_CreateRenderer(mainWindow, -1, EmulatorConfig::defaultRendererFlags);
#if defined(WRAPPER_MODE) && defined(CONSOLE_DISPLAY_FULLSCREEN)
	SDL_SetWindowFullscreen(mainWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
	isFullScreen = true;
#endif
	framebufferTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, GT_WIDTH, GT_HEIGHT * 2);
	SDL_SetTextureScaleMode(framebufferTexture, SDL_ScaleModeNearest);

#ifndef WASM_BUILD
	main_imgui_ctx = ImGui::CreateContext();
	main_implot_ctx = ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigViewportsNoAutoMerge = true;
	io.IniFilename = NULL;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
#ifdef WRAPPER_MODE
	{
		ImFontConfig font_cfg;
		font_cfg.PixelSnapH = true;
		font_cfg.OversampleH = 1;
		font_cfg.OversampleV = 1;
		font_cfg.FontDataOwnedByAtlas = false; // static embedded blob
		ImFont* proggy = io.Fonts->AddFontFromMemoryTTF(
			(void*)font_proggy_tiny_ttf, (int)font_proggy_tiny_ttf_len, 10.0f, &font_cfg);
		if(!proggy) {
			printf("Failed to load embedded ProggyTiny font; using default ImGui font\n");
			io.Fonts->AddFontDefault();
		}
	}
#endif
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(mainWindow, mainRenderer);
	ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual);
	ImGui_ImplSDLRenderer2_Init(mainRenderer);
#endif

	//Init joystick handler AFTER init imgui
	joysticks = new JoystickAdapter();
#ifdef WRAPPER_MODE
	LoadWrapperTabIcons();
	LoadWrapperSettings();
#endif

	#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	    rmask = 0xff000000;
	    gmask = 0x00ff0000;
	    bmask = 0x0000ff00;
	    amask = 0x000000ff;
	#else
	    rmask = 0x000000ff;
	    gmask = 0x0000ff00;
	    bmask = 0x00ff0000;
	    amask = 0xff000000;
	#endif

	randomize_vram();

#ifdef WRAPPER_MODE
	if(!rom_file_name) {
		showMenu = false;
		romLoaded = false;
		PauseEmulation();
		RefreshWrapperRomList();
	} else if(LoadRomFile(rom_file_name) == -1) {
		showMenu = false;
		romLoaded = false;
		PauseEmulation();
		RefreshWrapperRomList();
		printf("Failed to load ROM\n");
	} else {
		romLoaded = true;
		showMenu = false;
		ResumeEmulation();
	}
#else
	if(!rom_file_name || LoadRomFile(rom_file_name) == -1) {
		PauseEmulation();
#ifdef TINYFILEDIALOGS_H
		if(rom_file_name) {
			tinyfd_notifyPopup("Alert",
			"No ROM was loaded",
			"warning");
		}
#endif
		
	} else {
		ResumeEmulation();
	}
#endif

#ifdef WASM_BUILD

	emscripten_request_animation_frame_loop(mainloop, 0);
#else
	SDL_RaiseWindow(mainWindow);
	while(running) {
		mainloop(0, NULL);
	}
	joysticks->SaveBindings();
#endif

#ifndef WASM_BUILD
	if(savingThread.joinable()) {
		savingThread.join();
	}
#endif
	return 0;
}
