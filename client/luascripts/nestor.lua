local getopt = require('getopt')
local ansicolors = require('ansicolors')
local table = require('table')

--Copyright
copyright = ''
author = 'Wintrmvte'
version = 'v1'

-- Script description
desc = [[
    Performs nested key recovery against all/specific blocks
]]
example = [[
    -- Bruteforce block no. 3 with default key of type A
    1. x r nestor -n 3
]]
-- Usage info
usage = [[
x r nestor [-h] [-b <int>] [-f] [-x] [-1] [-k <str>] [-s] [-c]
]]
-- Arguments
arguments = [[
    -h           this help
    -n <int>     Target a single block 
    -k <hex>     Key for auth (default: ffffffffffff)
    -b           Key for auth is type B (default: A)
    -I           Target multiple blocks, but block specified as '-b' is first
    -f           Target first 8 blocks
    -x           Target every block (for Mifare 4k)
    -t           Target both key A and B (default: unspecified)
    -s           Add '--slow' flag randomly when testing a block
    -c           Use random, non-standard CPU instr set for bruteforce 
    -z m|s|a|2|5 Explicitly specify instructions set to use        
]]
-- Help function
local function help()
    print(copyright)
    print(author)
    print(version)
    print(desc)
    print(ansicolors.green..'Usage'..ansicolors.reset)
    print(usage)
    print(ansicolors.green..'Arguments'..ansicolors.reset)
    print(arguments)
end

local function main(args)
    -- Receive parameters
    for o, a in getopt.getopt(args, 'hbn:N:fxIs:k:ct') do
        if o == 'h' then return help() end
        if o == 'n' then blockno = a end
        if o == 'N' then knwnblk = a end
        if o == 'I' then bfirst = a end
        if o == 'f' then first_8 = true end
        if o == 'k' then key = a end
        if o == 'x' then extend = true end
        if o == 's' then slow = true end
        if o == 'c' then randinstr = true end
        if o == 'b' then input_b = true end
        if o == 't' then target_ab = true end
    end
    local instrs = {"m", "s", "a", "2", "5"}
    local bln = blockno or 3
    local known_blk = knwnblk or 0
    local blocks = {3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63}
    local key = key or "ffffffffffff"
    local cpu_instr = "n"
    local input_type = "-a"
    local targets=""
    if target_ab == true then
        targets = "--ta --tb"
    end
    if input_b == true then
        input_type = "-b"
    end
    if randinstr == true then
        cpu_instr = instrs[math.random(1, 5)]
    end
    if bln ~= 0 then
        blocks = {bln}
    end
    if first_8 == true then
        blocks = {3,7,11,15,19,23,27,31}
    end
    if extend == true then
        blocks = {3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63,67,71,75,79,83,87,91,95,99,103,107,111,115,119,123,127,143,159,175,191,207,223,239,255}
    end
    if (bfirst == true and bln ~= 0) then
        table.insert(blocks, 1, bln)
    end
    local args = string.format("%s %s -k %s --i%s",input_type,targets,key,cpu_instr)
    for k,v in ipairs(blocks) do
        if slow == true then
            local trueAndFalse = {true, false}
            local use_slow = trueAndFalse[math.random(1, 2)]
            if use_slow == true then
                args = args .. " --slow"
            end
        end
        local hardnested_cmd = string.format("hf mf hardnested --blk %s --tblk %s %s",known_blk,v,args)
        core.console(hardnested_cmd)
        core.clearCommandBuffer()
    end
end

main(args)