-- This module is loaded if a script calls require "gpackage".

local g = gollylib()

local m = {}

--------------------------------------------------------------------------------

function m.int(x)
    -- return integer part of given floating point number
    return x < 0 and math.ceil(x) or math.floor(x)
end

--------------------------------------------------------------------------------

function m.min(a)
	-- return minimum value in given array
    if #a == 0 then return nil end
    local n = a[1]
    for i = 2, #a do
        if a[i] < n then n = a[i] end
    end
    return n
end

--------------------------------------------------------------------------------

function m.max(a)
	-- return maximum value in given array
    if #a == 0 then return nil end
    local n = a[1]
    for i = 2, #a do
        if a[i] > n then n = a[i] end
    end
    return n
end

--------------------------------------------------------------------------------

function m.drawline(x1, y1, x2, y2, state)
	-- draw a line of cells from x1,y1 to x2,y2 using Bresenham's algorithm
	if not state then state = 1 end
    g.setcell(x1, y1, state)
    if x1 == x2 and y1 == y2 then return end
    
    local dx = x2 - x1
    local ax = math.abs(dx) * 2
    local sx = 1
    if dx < 0 then sx = -1 end
    
    local dy = y2 - y1
    local ay = math.abs(dy) * 2
    local sy = 1
    if dy < 0 then sy = -1 end
    
    if ax > ay then
        local d = ay - (ax / 2)
        while x1 ~= x2 do
            g.setcell(x1, y1, state)
            if d >= 0 then
                y1 = y1 + sy
                d = d - ax
            end
            x1 = x1 + sx
            d = d + ay
        end
    else
        local d = ax - (ay / 2)
        while y1 ~= y2 do
            g.setcell(x1, y1, state)
            if d >= 0 then
                x1 = x1 + sx
                d = d - ay
            end
            y1 = y1 + sy
            d = d + ax
        end
    end
    g.setcell(x2, y2, state)
end

--------------------------------------------------------------------------------

function m.getedges(r)
    -- return left, top, right, bottom edges of given cell rect
    return r[1], r[2], r[1]+r[3]-1, r[2]+r[4]-1
end

--------------------------------------------------------------------------------

function m.getminbox(cells)
	-- return a rect array with the minimal bounding box of given cell array
    local len = #cells
    if len < 2 then return {} end
    
    local minx = cells[1]
    local miny = cells[2]
    local maxx = minx
    local maxy = miny

	-- determine if cell array is one-state or multi-state
	local inc = 2
	if (len & 1) == 1 then inc = 3 end
    
    -- ignore padding int if present
    if (inc == 3) and (len % 3 == 1) then len = len - 1 end
    
    for x = 1, len, inc do
        if cells[x] < minx then minx = cells[x] end
        if cells[x] > maxx then maxx = cells[x] end
    end
    for y = 2, len, inc do
        if cells[y] < miny then miny = cells[y] end
        if cells[y] > maxy then maxy = cells[y] end
    end
    
    return {minx, miny, maxx - minx + 1, maxy - miny + 1}
end

--------------------------------------------------------------------------------

function m.rect(a)
    -- return a table that makes it easier to manipulate rectangles
    -- (emulates glife's rect class)
    local r = {}

	if #a == 0 then
		r.empty = true
	elseif #a == 4 then
		r.empty = false
		r.x  = a[1]
		r.y  = a[2]
		r.wd = a[3]
		r.ht = a[4]
		r.left   = r.x
		r.top    = r.y
		r.width  = r.wd
		r.height = r.ht
		if r.wd <= 0 then error("rect width must be > 0", 2) end
		if r.ht <= 0 then error("rect height must be > 0", 2) end
		r.right  = r.left + r.wd - 1
		r.bottom = r.top  + r.ht - 1
		r.visible = function ()
			return g.visrect( {r.x, r.y, r.wd, r.ht} )
		end
	else
		error("rect arg must be {} or {x,y,wd,ht}", 2)
	end
    
    return r
end

--------------------------------------------------------------------------------

-- create some useful synonyms:

-- for g.clear and g.advance
m.inside = 0
m.outside = 1

-- for g.flip
m.left_right = 0
m.top_bottom = 1
m.up_down = 1

-- for g.rotate
m.clockwise = 0
m.anticlockwise = 1

-- for g.setcursor (must match strings in Cursor Mode submenu)
m.draw    = "Draw"
m.pick    = "Pick"
m.select  = "Select"
m.move    = "Move"
m.zoomin  = "Zoom In"
m.zoomout = "Zoom Out"

-- transformation matrices
m.identity     = { 1,  0,  0,  1}
m.flip         = {-1,  0,  0, -1}
m.flip_x       = {-1,  0,  0,  1}
m.flip_y       = { 1,  0,  0, -1}
m.swap_xy      = { 0,  1,  1,  0}
m.swap_xy_flip = { 0, -1, -1,  0}
m.rcw          = { 0, -1,  1,  0}
m.rccw         = { 0,  1, -1,  0}

--------------------------------------------------------------------------------

-- create a metatable for all pattern tables
local mtp = {}

--------------------------------------------------------------------------------

function m.pattern(arg, x0, y0, A)
    -- return a table that makes it easier to manipulate patterns
    -- (emulates glife's pattern class)
    local p = {}
    
    setmetatable(p, mtp)
    
    if not arg then arg = {} end
	if not x0 then x0 = 0 end
	if not y0 then y0 = 0 end
	if not A then A = m.identity end
    
    if type(arg) == "table" then
    	p.array = {}
    	if getmetatable(arg) == mtp then
    		-- arg is a pattern
    		for i = 1, #arg.array do
    			p.array[i] = arg.array[i]
    		end
    	else
    		-- assume arg is a cell array
    		for i = 1, #arg do
    			p.array[i] = arg[i]
    		end
    	end
    elseif type(arg) == "string" then
    	p.array = g.parse(arg, x0, y0, table.unpack(A))
    else
    	error("1st arg of pattern must be a cell array, or a pattern, or a string", 2)
    end

    p.t = function (x, y, A)
        if not A then A = m.identity end
        return m.pattern( g.transform(p.array, x, y, table.unpack(A)) )
	end
	
	-- !!!
	
	p.put = function (x, y, A)
        -- paste pattern into current universe
        if not x then x = 0 end
        if not y then y = 0 end
        if not A then A = m.identity end
        g.putcells(p.array, x, y, table.unpack(A))
	end

    p.display = function (title, x, y, A)
        -- paste pattern into new universe and display it all
        if not title then title = "untitled" end
        if not x then x = 0 end
        if not y then y = 0 end
        if not A then A = m.identity end
        g.new(title)
        g.putcells(p.array, x, y, table.unpack(A))
        g.fit()
        g.setcursor(m.zoomin)
	end
	
	p.add = function (p1, p2)
		return m.pattern( g.join(p1.array, p2.array) )
	end
	
	p.evolve = function (p1, n)
		return m.pattern( g.evolve(p1.array, n) )
	end
	
	-- set metamethod so users can do pattern1 + pattern2
	mtp.__add = p.add

	-- set metamethod so users can do pattern[10] to get evolved pattern
	mtp.__index = p.evolve

	return p
end

--------------------------------------------------------------------------------

return m
