local getopt = require('getopt')
local ansicolors = require('ansicolors')

--Copyright
copyright = ''
author = 'Wintrmvte'
version = 'v1'

-- Script description
desc = [[
    Reads access data for all sector trailers
]]
example = [[
    -- Show ACL of specific block
    1. x r trailer_acl -b 3
]]
-- Usage info
usage = [[
x r acl [-h] [-b <int>] [-f] [-x]
]]
-- Arguments
arguments = [[
    -h          this help
    -b <int>    Show ACL for this block only
    -f          Show ACL for first 8 blocks
    -x          Show ACL of every block (for Mifare 4k)
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

-- Memory formatting
local function show_acl(bnum)
    local blocks = {3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63}
    if bnum != 0
        blocks = {bnum}
    end
    if bnum == -1
        blocks = {3,7,11,15,19,23,27,31}
    end
    if bnum == -2
        local blocks = {3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63,67,71,75,79,83,87,91,95,99,103,107,111,115,119,123,127,143,159,175,191,207,223,239,255}
    end
    for k,v in ipairs(blocks) do
        local cmd = string.format("hf mf egetblk --blk %s -v",v)
        core.console(cmd)
        core.clearCommandBuffer()
    end
end

local function main(args)
    -- Receive parameters
    for o, a in getopt.getopt(args, 'hb:fx') do
        if o == 'h' then return help() end
        if o == 'b' then blockno = a end
        if o == 'f' then first_8 = true end
        if o == 'x' then extend = true end
    end
    local bln = blockno or 0
    if first_8 == true then
        bln = -1
    end
    if extend == true then
        bln = -2
    end
    show_acl(bln)
end
main (args)
