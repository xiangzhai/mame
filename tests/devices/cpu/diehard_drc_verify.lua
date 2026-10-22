-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team
--
-- Real-game DRC verification helper for Sega ST-V diehard.
--
-- At fixed frames this dumps the emulated screen (hash + non-black pixel
-- count) and the contents of the work RAM, sound RAM, backup RAM, VDP1/
-- VDP2 and SCU registers.  The companion script runs this with the native
-- LoongArch64 DRC backend and with -drc_use_c and compares the results.
--
-- DRC_VERIFY_OUT must point at an existing output directory.

local machine = manager.machine
local screen = machine.screens:at(1)
local space = machine.devices[":maincpu"].spaces["program"]
local outdir = os.getenv("DRC_VERIFY_OUT")

if not outdir then
	error("DRC_VERIFY_OUT is not set")
end

local frames = { 120, 180, 240 }
local frame = 0
local next_frame = 1

local regions = {
	{ "workram_l",   0x00200000, 0x100000 },
	{ "workram_h",   0x06000000, 0x100000 },
	{ "sound_ram",   0x05a00000, 0x100000 },
	{ "backup_ram",  0x00180000, 0x008000 },
	{ "vdp1_regs",   0x05d00000, 0x000020 },
	{ "vdp2_regs",   0x05f80000, 0x000100 },
	{ "vdp2_cram",   0x05f00000, 0x001000 },
	{ "scu_regs",    0x05fe0000, 0x0000d0 },
}

local function hash32(s)
	local h = 2166136261
	for i = 1, #s do
		h = (h ~ s:byte(i)) * 16777619 & 0xffffffff
	end
	return h
end

local function nonblack_pixels(pixels)
	local count = 0
	for i = 1, #pixels, 4 do
		local v = string.unpack("<I4", pixels, i)
		if (v & 0x00ffffff) ~= 0 then
			count = count + 1
		end
	end
	return count
end

local function dump_region(name, start_address, length)
	local filename = string.format("%s/mem_%04d_%s.bin", outdir, frame, name)
	local file = assert(io.open(filename, "wb"))
	local chunk = {}
	for address = start_address, start_address + length - 4, 4 do
		local value = space:read_u32(address)
		chunk[#chunk + 1] = string.char(
				value & 0xff,
				(value >> 8) & 0xff,
				(value >> 16) & 0xff,
				(value >> 24) & 0xff)
		if #chunk == 4096 then
			file:write(table.concat(chunk))
			chunk = {}
		end
	end
	if #chunk ~= 0 then
		file:write(table.concat(chunk))
	end
	file:close()
end

local function dump_frame()
	local pixels, width, height = screen:pixels()
	local summary = assert(io.open(string.format("%s/screen_%04d.txt", outdir, frame), "w"))
	summary:write(string.format("frame=%d width=%d height=%d hash=%08x nonblack=%d\n",
			frame, width, height, hash32(pixels), nonblack_pixels(pixels)))
	summary:close()

	for _, region in ipairs(regions) do
		dump_region(region[1], region[2], region[3])
	end
end

emu.register_frame_done(function ()
	frame = frame + 1
	if next_frame <= #frames and frame == frames[next_frame] then
		dump_frame()
		next_frame = next_frame + 1
		if next_frame > #frames then
			machine:exit()
		end
	end
end)
