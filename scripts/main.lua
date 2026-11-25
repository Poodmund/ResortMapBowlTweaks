-- SPDX-License-Identifier: 0BSD
-- ResortMapBowlTweaks v1.1.0: in-game Interface settings.
-- The four visual options are committed only through the game's Apply action.
-- The native component reloads the committed INI before rebuilding Resort Map.

local PREFIX = "[ResortMapBowlTweaks.UI] "
local LOG_NAME = "ResortMapBowlTweaks_UI.log"
local automatic = true
local automatic_positioning = true
local monitor_interval_ms = 500
local ticks = 0
local backend = "none"
local installed, sessions, pending_apply = {}, {}, {}
local active_panel_key = nil
local hooks = { apply=false, back=false, reset=false }
local hook_attempts = 0

local definitions = {
    { key="lines", label="Bowl line style", values={"Dashed", "Solid", "Hidden"}, default="Dashed" },
    { key="dash_length", label="Dash length", values={}, default="7" },
    { key="gap_length", label="Dash spacing", values={}, default="15" },
    { key="trail_name_text", label="Bowl name label style", values={"Default", "Inverted", "Box"}, default="Default" },
}
for n=3,30 do definitions[2].values[#definitions[2].values+1] = tostring(n) end
for n=4,60 do definitions[3].values[#definitions[3].values+1] = tostring(n) end
local allowed = {}
local committed = {}
for _, def in ipairs(definitions) do
    allowed[def.key] = {}
    for _, value in ipairs(def.values) do allowed[def.key][value] = true end
    committed[def.key] = def.default
end

local function copy_values(value)
    local result = {}
    for _, def in ipairs(definitions) do result[def.key] = value[def.key] end
    return result
end
local function equal_values(a,b)
    if not a or not b then return false end
    for _, def in ipairs(definitions) do
        if a[def.key] ~= b[def.key] then return false end
    end
    return true
end
local function describe(value)
    if not value then return "<nil>" end
    local items = {}
    for _, def in ipairs(definitions) do
        items[#items+1] = def.key .. "=" .. tostring(value[def.key])
    end
    return table.concat(items, " ")
end
local function safe(fn)
    local ok, result = pcall(fn)
    if ok then return result end
    return nil
end
local function valid(obj)
    return obj ~= nil and safe(function() return obj:IsValid() end) == true
end
local function obj_name(obj)
    if not valid(obj) then return "<invalid>" end
    return safe(function() return obj:GetFName():ToString() end) or "<unnamed>"
end
local function class_name(obj)
    return obj_name(safe(function() return obj:GetClass() end))
end
local function full_name(obj)
    return valid(obj) and (safe(function() return obj:GetFullName() end) or obj_name(obj)) or "<invalid>"
end
local function property(obj,key)
    if not valid(obj) then return nil end
    return safe(function() return obj:GetPropertyValue(key) end)
end
local function parent(obj)
    local p = valid(obj) and safe(function() return obj:GetParent() end) or nil
    return valid(p) and p or nil
end
local ui_debug_enabled = false
local CONFIG_PATH, CONFIG_SOURCE
local function report(message)
    local critical = message:match("^INI_SAVE_FAILED:") or
        message:match("^MONITOR_ERROR:") or message:match("^NO_INSERT:")
    if not ui_debug_enabled then
        if critical then print(PREFIX .. message .. "\n") end
        return
    end
    print(PREFIX .. message .. "\n")
    if CONFIG_PATH and io and io.open then
        local dir=CONFIG_PATH:match("^(.+/)[^/]+$")
        local f = dir and safe(function() return io.open(dir .. "logs/" .. LOG_NAME, "a") end) or nil
        if f then safe(function() f:write(message,"\n"); f:close() end) end
    end
end
-- Resolve the owning mod relative to this script, whether UE4SS supplies
-- an absolute path or a relative path beginning with "mods/".
-- Resolve paths from the script location, not the game process working directory.
local function resolve_ini_path()
    local source=safe(function() return debug.getinfo(1,"S").source end)
    if type(source)~="string" or source:sub(1,1)~="@" then
        return nil, "Lua source path is unavailable: "..tostring(source)
    end
    local normalized=source:sub(2):gsub("\\","/")
    -- UE4SS may report "Scripts". Match the path suffix
    -- without changing the original casing of the resolved Windows path.
    local suffix="/scripts/main.lua"
    if normalized:lower():sub(-#suffix)~=suffix then
        return nil, "Unexpected Lua source location: "..normalized
    end
    local mod_dir=normalized:sub(1,#normalized-#suffix)
    local mod_name=mod_dir:match("([^/]+)$")
    if not mod_name or mod_name:lower()~="resortmapbowltweaks" then
        return nil, "Unexpected Lua mod directory: "..mod_dir
    end
    -- The UI script and native DLL share the same mod directory.
    return mod_dir.."/ResortMapBowlTweaks.ini", normalized
end
CONFIG_PATH, CONFIG_SOURCE=resolve_ini_path()
local INI_KEYS={"lines","dash_length","gap_length","trail_name_text"}
local function trimmed(value)
    return tostring(value):match("^%s*(.-)%s*$")
end
local function normalise_ini_value(key,value)
    if not allowed[key] then return nil end
    local candidate=trimmed(value):lower()
    -- Ignore INI-style semicolon comments, as the C++ loader does.
    candidate=trimmed(candidate:match("^(.-);") or candidate)
    if key=="dash_length" or key=="gap_length" then
        local n=tonumber(candidate)
        if not n or n%1~=0 then return nil end
        candidate=tostring(n)
    else
        candidate=candidate:sub(1,1):upper()..candidate:sub(2)
    end
    return allowed[key][candidate] and candidate or nil
end
local function read_ini_text()
    if not CONFIG_PATH then return nil, CONFIG_SOURCE or "INI path unavailable" end
    if not io or type(io.open)~="function" then return nil,"Lua file I/O unavailable" end
    local f,err=io.open(CONFIG_PATH,"rb")
    if not f then return nil,err or "INI not found" end
    local body=f:read("*a")
    f:close()
    if type(body)~="string" then return nil,"INI read failed" end
    if #body>262144 then return nil,"INI exceeds 256 KiB safety limit" end
    return body
end
local function parse_ini_values(body)
    local result=copy_values(committed)
    local in_section=false
    for line in (body.."\n"):gmatch("([^\n]*)\n") do
        line=line:gsub("\r$","")
        local section=line:match("^%s*%[([^%]]+)%]%s*$")
        if section then
            in_section=trimmed(section):lower()=="resortmap"
        elseif in_section then
            local key,value=line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
            if key then
                key=key:lower()
                local parsed=normalise_ini_value(key,value)
                if parsed then result[key]=parsed end
            end
        end
    end
    return result
end
-- Update only the four UI-controlled values. Preserve unrelated options,
-- existing comments, and any future configuration keys.
local function update_ini_text(body,values)
    local newline=body:find("\r\n",1,true) and "\r\n" or "\n"
    local original_trailing_newline=body:sub(-1)=="\n"
    local lines={}
    for line in (body.."\n"):gmatch("([^\n]*)\n") do
        lines[#lines+1]=line:gsub("\r$","")
    end
    if original_trailing_newline then table.remove(lines) end
    local in_section=false
    local seen,insert_pos,found_section={},nil,false
    for i,line in ipairs(lines) do
        local section=line:match("^%s*%[([^%]]+)%]%s*$")
        if section then
            if in_section and not insert_pos then insert_pos=i end
            in_section=trimmed(section):lower()=="resortmap"
            if in_section then found_section=true; insert_pos=nil end
        elseif in_section then
            local indent,key=line:match("^(%s*)([%w_]+)%s*=")
            if key then
                key=key:lower()
                if allowed[key] then
                    if not seen[key] then
                        lines[i]=indent..key.." = "..values[key]:lower()
                        seen[key]=true
                    else
                        -- Drop duplicate assignments, so last-wins parsing cannot
                        -- silently override the player's applied selection.
                        lines[i]="; Duplicate "..key.." assignment retired by Settings UI"
                    end
                end
            end
        end
    end
    if not found_section then
        if #lines>0 and lines[#lines]~="" then lines[#lines+1]="" end
        lines[#lines+1]="[ResortMap]"
        insert_pos=#lines+1
    end
    if not insert_pos then insert_pos=#lines+1 end
    for _,key in ipairs(INI_KEYS) do
        if not seen[key] then
            table.insert(lines,insert_pos,key.." = "..values[key]:lower())
            insert_pos=insert_pos+1
        end
    end
    return table.concat(lines,newline)..(original_trailing_newline and newline or "\n")
end
local function save_ini_values(values)
    for _,def in ipairs(definitions) do
        if not allowed[def.key][values[def.key]] then return false,"invalid value for "..def.key end
    end
    local old,err=read_ini_text()
    if not old then return false,"Cannot read existing INI: "..tostring(err) end
    local updated=update_ini_text(old,values)
    if updated==old then return true,"unchanged" end
    local temp=CONFIG_PATH..".ui-pending.tmp"
    local backup=CONFIG_PATH..".bak"
    local f,open_err=io.open(temp,"wb")
    if not f then return false,"Cannot create temporary INI: "..tostring(open_err) end
    local ok,write_err=f:write(updated)
    local closed,close_err=f:close()
    if not ok or not closed then
        os.remove(temp)
        return false,"Temporary INI write failed: "..tostring(write_err or close_err)
    end
    -- Windows rename() normally refuses to replace a destination. Keep the
    -- previous INI in a backup while promoting the fully-written temporary file.
    os.remove(backup)
    local old_moved,move_err=os.rename(CONFIG_PATH,backup)
    if not old_moved then
        os.remove(temp)
        return false,"INI backup rename failed: "..tostring(move_err)
    end
    local promoted,promote_err=os.rename(temp,CONFIG_PATH)
    if not promoted then
        local restored,restore_err=os.rename(backup,CONFIG_PATH)
        os.remove(temp)
        return false,"INI promotion failed: "..tostring(promote_err)
            .."; backup_restored="..tostring(restored)
            .."; restore_error="..tostring(restore_err)
    end
    -- Keep .bak as a recovery copy of the configuration before the last Apply.
    return true,"saved"
end
local function initialise_from_ini()
    -- Only diagnostic mode produces UI log entries. A missing INI is handled
    -- by the production C++ component's first-run generator.
    local raw,err=read_ini_text()
    if raw then
        local v=raw:match("[\r\n]debug%s*=%s*([^\r\n;]+)")
        ui_debug_enabled = v ~= nil and (trimmed(v):lower()=="true" or trimmed(v)=="1" or trimmed(v):lower()=="yes")
    end
    report("INI_PATH: script="..tostring(CONFIG_SOURCE).." target="..tostring(CONFIG_PATH)
        .." io_open="..tostring(io~=nil and type(io.open)=="function"))
    if not raw then
        report("INI_LOAD: unavailable="..tostring(err).." using compiled defaults")
        return
    end
    committed=parse_ini_values(raw)
    report("INI_LOAD: file="..CONFIG_PATH.." values="..describe(committed))
end
initialise_from_ini()

local function enumerate(name)
    local result = safe(function() return FindAllOf(name) end)
    return type(result)=="table" and result or {}
end
local function owning_menu(panel)
    local node = panel
    for _=1,12 do
        if not valid(node) then break end
        if class_name(node)=="SettingsMenuWidget" then return node end
        node = safe(function() return node:GetOuter() end)
    end
    return nil
end
local function row_of_combo(combo,vbox)
    local node = combo
    for _=1,10 do
        local p = parent(node)
        if not valid(p) then return nil,-1 end
        if full_name(p)==full_name(vbox) then
            local count = safe(function() return vbox:GetChildrenCount() end) or 0
            for i=0,count-1 do
                local child = safe(function() return vbox:GetChildAt(i) end)
                if full_name(child)==full_name(node) then return node,i end
            end
            return node,-1
        end
        node = p
    end
    return nil,-1
end
local function find_active_panel()
    local candidates = {}
    for order,panel in ipairs(enumerate("InterfaceSettingsPanel")) do
        if valid(panel) then
            local native = property(panel,"TrailRatingStyleCombo")
            local node = native
            for _=1,10 do
                if not valid(node) then break end
                local p = parent(node)
                if not valid(p) then break end
                if obj_name(p)=="InterfaceContentVBox" then
                    local count = safe(function() return p:GetChildrenCount() end)
                    local menu = owning_menu(panel)
                    if valid(menu) and type(count)=="number" and count>=1 and count<100 then
                        local viewport = safe(function() return menu:IsInViewport() end)
                        local visible = safe(function() return panel:IsVisible() end)
                        local priority = viewport == true and 100 or viewport == false and 0 or 10
                        if visible == true then priority=priority+1 end
                        candidates[#candidates+1] = {panel=panel, vbox=p, native=native,
                            menu=menu, order=order, viewport=viewport, priority=priority}
                    end
                    break
                end
                node = p
            end
        end
    end
    table.sort(candidates,function(a,b)
        return a.priority~=b.priority and a.priority>b.priority or
            a.priority==b.priority and a.order>b.order
    end)
    local best = candidates[1]
    -- Never insert into an explicitly off-screen Settings menu.
    if best and best.viewport==false then return nil,#candidates end
    return best,#candidates
end
local function new_session(menu,key)
    local session=sessions[key]
    if not session or session.closed then
        session={menu=menu, draft=copy_values(committed), closed=false,
            enable_attempts=0}
        sessions[key]=session
        report("TRANSACTION_OPEN: menu=" .. key .. " restored=" .. describe(session.draft))
    end
    return session
end
local function record_for_menu(key)
    for _,rec in pairs(installed) do
        if rec.menu_key==key and valid(rec.panel) and valid(rec.container)
            and valid(parent(rec.container)) then return rec end
    end
    return nil
end
local function get_active_cached()
    local rec=active_panel_key and installed[active_panel_key] or nil
    if not rec or not valid(rec.panel) or not valid(rec.container)
        or not valid(rec.menu) or not valid(parent(rec.container)) then return nil end
    if safe(function() return rec.menu:IsInViewport() end)==false then return nil end
    return rec
end
local function get_active_record()
    local rec=get_active_cached()
    if rec then return rec end
    local candidate=find_active_panel()
    if candidate then
        rec=installed[full_name(candidate.panel)]
        if rec then active_panel_key=full_name(candidate.panel) end
    end
    return rec
end
local function uclass(name)
    return safe(function() return StaticFindObject("/Script/UMG."..name) end)
end
local function construct(cls,outer)
    if not valid(cls) or not valid(outer) then return nil end
    local obj=safe(function() return StaticConstructObject(cls,outer,0) end)
    return valid(obj) and obj or nil
end
local function selected(choice)
    if not valid(choice) then return nil end
    local raw=safe(function() return choice:GetSelectedOption() end)
    if type(raw)=="string" then return raw end
    return safe(function() return raw:ToString() end)
end

-- Optional visual-property copying. Missing properties or unsupported struct
-- assignments are ignored; never modify the native reference widget.
local function copy_props(dst,src,props)
    local copied=0
    if not valid(dst) or not valid(src) then return 0 end
    for _,name in ipairs(props) do
        local value=property(src,name)
        if value~=nil then
            local ok=pcall(function() dst:SetPropertyValue(name,value) end)
            if ok then copied=copied+1 end
        end
    end
    return copied
end
local function reference_parts(native,vbox)
    local size=parent(native)
    local horizontal=parent(size)
    local border=parent(horizontal)
    if not valid(border) or full_name(parent(border))~=full_name(vbox) then return nil end
    local label_size=safe(function() return horizontal:GetChildAt(0) end)
    local text=valid(label_size) and safe(function() return label_size:GetChildAt(0) end) or nil
    return {border=border,horizontal=horizontal,label_size=label_size,
        label=text,choice_size=size,choice=native}
end
local function decorate_row(part,reference)
    if not reference then return 0 end
    local count=0
    count=count+copy_props(part.border,reference.border,{"Brush","BrushColor","Padding","ContentColorAndOpacity"})
    count=count+copy_props(part.label,reference.label,{"Font","ColorAndOpacity","ShadowOffset","ShadowColorAndOpacity","Justification","AutoWrapText"})
    count=count+copy_props(part.label_size,reference.label_size,{"WidthOverride","HeightOverride","MinDesiredWidth","MinDesiredHeight","MaxDesiredWidth","MaxDesiredHeight",
        "bOverride_WidthOverride","bOverride_HeightOverride","bOverride_MinDesiredWidth","bOverride_MinDesiredHeight","bOverride_MaxDesiredWidth","bOverride_MaxDesiredHeight"})
    count=count+copy_props(part.choice_size,reference.choice_size,{"WidthOverride","HeightOverride","MinDesiredWidth","MinDesiredHeight","MaxDesiredWidth","MaxDesiredHeight",
        "bOverride_WidthOverride","bOverride_HeightOverride","bOverride_MinDesiredWidth","bOverride_MinDesiredHeight","bOverride_MaxDesiredWidth","bOverride_MaxDesiredHeight"})
    count=count+copy_props(part.choice,reference.choice,{"WidgetStyle","ItemStyle","ScrollBarStyle", "ForegroundColor", "ContentPadding", "HasDownArrow", "MaxListHeight", "ColorAndOpacity", "RenderOpacity", "EnableGamepadNavigationMode"})
    -- Copy compatible UMG appearance properties; Slate-specific styling
    -- remains controlled by the game.
    return count, false
end
local selector_style_fields={"WidgetStyle","ItemStyle","ScrollBarStyle","ForegroundColor",
    "ContentPadding","HasDownArrow","MaxListHeight","ColorAndOpacity",
    "RenderOpacity","EnableGamepadNavigationMode"}
local function copy_selector_early(choice,reference)
    if not reference or not valid(reference.choice) then return 0,"no_reference" end
    return copy_props(choice,reference.choice,selector_style_fields),"not_attempted"
end
local function copy_slot_properties(dst,src)
    return copy_props(dst,src,{"Padding","Size","HorizontalAlignment","VerticalAlignment"})
end
local function new_row(tree, native_class, fallback_class, reference, def, init)
    local border=construct(uclass("Border"),tree)
    local horizontal=construct(uclass("HorizontalBox"),tree)
    local label_size=construct(uclass("SizeBox"),tree)
    local choice_size=construct(uclass("SizeBox"),tree)
    local label=construct(uclass("TextBlock"),tree)
    local choice=construct(native_class,tree)
    if not choice then choice=construct(fallback_class,tree) end
    if not (border and horizontal and label_size and choice_size and label and choice) then
        return nil,"construction_failed"
    end
    local early_copied,early_setter=copy_selector_early(choice,reference)
    local ok,err=pcall(function()
        label:SetText(FText(def.label))
        for _,value in ipairs(def.values) do choice:AddOption(value) end
        choice:SetSelectedOption(init)
        border:AddChild(horizontal)
        horizontal:AddChildToHorizontalBox(label_size)
        horizontal:AddChildToHorizontalBox(choice_size)
        label_size:AddChild(label)
        choice_size:AddChild(choice)
    end)
    if not ok then return nil,"populate_failed: "..tostring(err) end
    local part={border=border,horizontal=horizontal,label_size=label_size,
        choice_size=choice_size,label=label,choice=choice,def=def,last_value=init,
        style_early_copied=early_copied,style_early_setter=early_setter}
    part.style_properties,part.style_refreshed=decorate_row(part,reference)
    -- HorizontalBox slots control Fill/Auto sizing and alignment. Copying only
    -- SizeBox WidthOverride gives a visually narrower dropdown than the game row.
    part.horizontal_slot_properties=0
    if reference then
        part.horizontal_slot_properties=part.horizontal_slot_properties+
            copy_slot_properties(property(part.label_size,"Slot"),property(reference.label_size,"Slot"))
        part.horizontal_slot_properties=part.horizontal_slot_properties+
            copy_slot_properties(property(part.choice_size,"Slot"),property(reference.choice_size,"Slot"))
    end
    return part
end
local function sync_widgets(rec,values)
    if not rec then return end
    for _,def in ipairs(definitions) do
        local part=rec.parts[def.key]
        if part and valid(part.choice) then
            local value=values[def.key]
            if allowed[def.key][value] then
                safe(function() part.choice:SetSelectedOption(value) end)
                part.last_value=value
            end
        end
    end
    local dashed=values.lines=="Dashed"
    for _,id in ipairs({"dash_length","gap_length"}) do
        local part=rec.parts[id]
        if part and valid(part.choice) then
            safe(function() part.choice:SetIsEnabled(dashed) end)
        end
    end
end
local function dirty(session)
    return session and not session.closed and not equal_values(session.draft,committed)
end
local function enable_apply(rec,source)
    if not rec then return end
    local session=sessions[rec.menu_key]
    if not dirty(session) then return end
    local button=property(rec.menu,"ApplyButton")
    if not valid(button) then return end
    if safe(function() return button:GetIsEnabled() end)~=false then return end
    local ok,err=pcall(function() button:SetIsEnabled(true) end)
    local enabled=safe(function() return button:GetIsEnabled() end)
    session.enable_attempts=(session.enable_attempts or 0)+1
    if session.enable_attempts<=5 or session.enable_attempts%20==0 then
        report("APPLY_ENABLE: source="..source.." menu="..rec.menu_key
            .." enabled="..tostring(enabled).." attempts="..session.enable_attempts
            ..(ok and "" or " error="..tostring(err)))
    end
end
local function capture(rec,source)
    if not rec or not valid(rec.container) or not valid(parent(rec.container)) then return end
    local session=new_session(rec.menu,rec.menu_key)
    local changed=false
    for _,def in ipairs(definitions) do
        local part=rec.parts[def.key]
        if part and valid(part.choice) then
            local value=selected(part.choice)
            if allowed[def.key][value] then
                if value~=part.last_value then
                    part.last_value=value
                    if session.draft[def.key]~=value then
                        report("PENDING_CHANGED: source="..source.." menu="..rec.menu_key
                            .." setting="..def.key.." from="..tostring(session.draft[def.key])
                            .." to="..value)
                        session.draft[def.key]=value
                        changed=true
                    end
                end
            end
        end
    end
    if changed then
        local dashed=session.draft.lines=="Dashed"
        for _,id in ipairs({"dash_length","gap_length"}) do
            local part=rec.parts[id]
            if part and valid(part.choice) then
                safe(function() part.choice:SetIsEnabled(dashed) end)
            end
        end
        report("PENDING: dirty="..tostring(dirty(session)).." "..describe(session.draft))
    end
    enable_apply(rec,source)
end
local function discard(key,reason)
    local session=sessions[key]
    if not session or session.closed then return end
    local old=describe(session.draft)
    session.draft=copy_values(committed)
    session.closed=true
    sync_widgets(record_for_menu(key),committed)
    report("DISCARD: source="..reason.." menu="..key
        .." previous="..old.." restored="..describe(committed))
end

local install_hooks
local add_rows
local reorder_after_trail
local function attach_rows(source)
    local candidate,count=find_active_panel()
    if not candidate then
        if source=="manual" then report("NO_INSERT: active Interface panel not found") end
        return
    end
    local panel,vbox,native,menu=candidate.panel,candidate.vbox,candidate.native,candidate.menu
    local key=full_name(panel)
    local menu_key=full_name(menu)
    if install_hooks and (not hooks.apply or not hooks.back or not hooks.reset) then
        install_hooks("menu_ready")
    end
    local existing=installed[key]
    if existing and valid(existing.container)
        and full_name(parent(existing.container))==full_name(vbox) then
        local reopened=sessions[menu_key] and sessions[menu_key].closed
        local session=new_session(menu,menu_key)
        if reopened then sync_widgets(existing,session.draft) end
        active_panel_key=key
        if source=="manual" then report("ALREADY_INSERTED: panel="..key) end
        return
    end
    local tree=property(panel,"WidgetTree")
    local native_cls=safe(function() return native:GetClass() end)
    local fallback=uclass("ComboBoxString")
    if not valid(tree) or not valid(fallback) then
        report("NO_INSERT: no widget tree / combo class")
        return
    end
    local session=new_session(menu,menu_key)
    local container=construct(uclass("VerticalBox"),tree)
    if not container then report("NO_INSERT: cannot construct container") return end
    local reference=reference_parts(native,vbox)
    local parts={}
    local total_styles=0
    local total_early_styles=0
    local early_setters={}
    local total_horizontal_slots=0
    local refreshed_styles=0
    for _,def in ipairs(definitions) do
        local part,err=new_row(tree,native_cls,fallback,reference,def,session.draft[def.key])
        if not part then
            report("NO_INSERT: row="..def.key.." error="..tostring(err))
            return
        end
        local ok=pcall(function() container:AddChildToVerticalBox(part.border) end)
        if not ok or full_name(parent(part.border))~=full_name(container) then
            report("NO_INSERT: cannot attach row="..def.key)
            return
        end
        if reference and valid(property(reference.border,"Slot")) then
            total_styles=total_styles+copy_slot_properties(property(part.border,"Slot"),property(reference.border,"Slot"))
        end
        total_styles=total_styles+(part.style_properties or 0)
        total_early_styles=total_early_styles+(part.style_early_copied or 0)
        early_setters[#early_setters+1]=part.style_early_setter or "unknown"
        total_horizontal_slots=total_horizontal_slots+(part.horizontal_slot_properties or 0)
        if part.style_refreshed then refreshed_styles=refreshed_styles+1 end
        parts[def.key]=part
    end
    local before=safe(function() return vbox:GetChildrenCount() end) or -1
    local ok,err=pcall(function() vbox:AddChildToVerticalBox(container) end)
    if not ok or full_name(parent(container))~=full_name(vbox) then
        report("NO_INSERT: cannot attach settings group: "..tostring(err))
        return
    end
    local group_slot_copied=0
    if reference then
        group_slot_copied=copy_slot_properties(property(container,"Slot"),property(reference.border,"Slot"))
    end
    local rec={panel=panel,menu=menu,menu_key=menu_key,vbox=vbox,native=native,
        container=container,parts=parts,positioned=false,
        position_attempted=false,inserted_tick=ticks}
    installed[key]=rec
    active_panel_key=key
    sync_widgets(rec,session.draft)
    local _,trail_index=row_of_combo(native,vbox)
    report("ROWS_ATTACHED: source="..source.." panel="..key
        .." context="..class_name(native).." candidate_panels="..tostring(count)
        .." before="..tostring(before)
        .." after="..tostring(safe(function() return vbox:GetChildrenCount() end))
        .." trail_row_index="..tostring(trail_index)
        .." native_style_properties_copied="..tostring(total_styles)
        .." early_selector_style_copies="..tostring(total_early_styles)
        .." early_style_setters="..table.concat(early_setters,",")
        .." horizontal_slot_properties_copied="..tostring(total_horizontal_slots)
        .." group_slot_properties_copied="..tostring(group_slot_copied)
        .." style_refresh_calls="..tostring(refreshed_styles)
        .." mode=append_then_auto_position"
        .." restored="..describe(session.draft))
end
add_rows=attach_rows

-- Place the mod row group beneath Trail Rating Style. Reordering replaces
-- VerticalBox slots, so preserve compatible layout properties on reattachment.
reorder_after_trail=function(rec,source)
    rec=rec or get_active_record()
    source=source or "manual"
    if not rec then report("REORDER_SKIP: no active inserted group") return end
    if rec.positioned then
        if source=="manual" then report("REORDER_SKIP: already positioned") end
        return
    end
    -- Do not repeat a failed attempt every timer tick.
    rec.position_attempted=true
    local vbox=rec.vbox
    local row,index=row_of_combo(rec.native,vbox)
    local count=safe(function() return vbox:GetChildrenCount() end) or 0
    local group_index=-1
    for i=0,count-1 do
        local child=safe(function() return vbox:GetChildAt(i) end)
        if full_name(child)==full_name(rec.container) then group_index=i;break end
    end
    if not valid(row) or index<0 or group_index<=index+1 or count>100 then
        report("REORDER_SKIP: source="..source.." indexes trail="..tostring(index).." group="..tostring(group_index))
        return
    end
    local tail={}
    for i=index+1,group_index-1 do
        local child=safe(function() return vbox:GetChildAt(i) end)
        if not valid(child) or child==rec.container then
            report("REORDER_SKIP: unsafe native trailing child")
            return
        end
        local slot=property(child,"Slot")
        tail[#tail+1]={child=child,slot=slot}
    end
    -- Removing the vanilla suffix lets the appended group move immediately
    -- after Trail Rating. Re-appending the suffix preserves its visible order.
    local removed={}
    for i=#tail,1,-1 do
        local item=tail[i]
        local ok,result=pcall(function() return vbox:RemoveChild(item.child) end)
        if not ok or result==false then
            report("REORDER_FAILED: remove position="..i.." error="..tostring(result))
            -- Best-effort restoration; do not attempt more modifications.
            for j=#removed,1,-1 do
                safe(function() vbox:AddChildToVerticalBox(removed[j].child) end)
            end
            return
        end
        removed[#removed+1]=item
    end
    local restored=0
    local copied=0
    for _,item in ipairs(tail) do
        local slot=safe(function() return vbox:AddChildToVerticalBox(item.child) end)
        if not valid(slot) then
            report("REORDER_FAILED: readd native child "..full_name(item.child))
            return
        end
        copied=copied+copy_slot_properties(slot,item.slot)
        restored=restored+1
    end
    local _,new_index=row_of_combo(rec.native,vbox)
    local group=safe(function() return vbox:GetChildAt(new_index+1) end)
    rec.positioned=valid(group) and full_name(group)==full_name(rec.container)
    report("REORDER_RESULT: source="..source.." menu="..rec.menu_key.." positioned="..tostring(rec.positioned)
        .." moved_native_rows="..restored
        .." slot_fields_copied="..copied
        .." original_count="..count
        .." final_count="..tostring(safe(function() return vbox:GetChildrenCount() end)))
end

local MENU_PATH="/Script/SkiResortSimulator.SettingsMenuWidget:"
local function hook_menu(Context)
    local menu=safe(function() return Context:get() end)
    return valid(menu) and menu or nil, valid(menu) and full_name(menu) or nil
end
local function on_apply_pre(Context)
    local menu,key=hook_menu(Context)
    if not key then return end
    local rec=record_for_menu(key)
    if not rec then return end
    capture(rec,"apply_pre")
    local session=sessions[key]
    if session and not session.closed then
        pending_apply[key]=copy_values(session.draft)
        report("APPLY_PRE: menu="..key.." pending="..describe(session.draft))
    end
end
local function on_apply_post(Context)
    local menu,key=hook_menu(Context)
    if not key then return end
    local intent=pending_apply[key]
    pending_apply[key]=nil
    if not intent then return end
    for _,def in ipairs(definitions) do
        if not allowed[def.key][intent[def.key]] then
            report("APPLY_ABORT: invalid="..def.key)
            return
        end
    end
    local before=describe(committed)
    if equal_values(committed,intent) then
        report("APPLY_NO_CHANGE: menu="..key.." values="..describe(intent))
    else
        local written,status=save_ini_values(intent)
        if not written then
            report("INI_SAVE_FAILED: menu="..key.." reason="..tostring(status)
                .." committed_unchanged="..describe(committed))
            local session=sessions[key]
            if session then
                session.draft=copy_values(committed)
                sync_widgets(record_for_menu(key),committed)
            end
            return
        end
        report("INI_SAVE: menu="..key.." result="..tostring(status))
    end
    committed=copy_values(intent)
    for k,session in pairs(sessions) do
        session.draft=copy_values(committed)
        session.closed=k~=key
        sync_widgets(record_for_menu(k),committed)
    end
    report("APPLY_COMMIT: menu="..key.." previous="..before
        .." committed="..describe(committed)
        .." (saved to INI; native DLL reloads on Resort Map rebuild)")
end
local function on_back_pre(Context)
    local _,key=hook_menu(Context)
    if key and not pending_apply[key] then discard(key,"back_button") end
end
local function on_reset_pre(Context)
    local _,key=hook_menu(Context)
    if key then report("RESET_OBSERVED: menu="..key.." mod values unchanged") end
end
install_hooks=function(source)
    if hooks.apply and hooks.back and hooks.reset then return end
    if type(RegisterHook)~="function" or hook_attempts>=3 then return end
    hook_attempts=hook_attempts+1
    local function try_hook(which,path,pre,post)
        if hooks[which] then return end
        local ok,id=pcall(function()
            if post then return RegisterHook(path,pre,post) end
            return RegisterHook(path,pre)
        end)
        hooks[which]=ok and id~=nil
        report("HOOK_"..which:upper()..": "..(hooks[which] and "registered" or "unavailable")
            .." source="..source.." attempt="..hook_attempts)
    end
    try_hook("apply",MENU_PATH.."OnApplyClicked",on_apply_pre,on_apply_post)
    try_hook("back",MENU_PATH.."OnBackButtonClicked",on_back_pre,nil)
    try_hook("reset",MENU_PATH.."OnResetClicked",on_reset_pre,nil)
end
local function monitor_tick()
    ticks=ticks+1
    local rec=get_active_cached()
    if rec then
        capture(rec,"monitor")
        if automatic_positioning and not rec.positioned
            and not rec.position_attempted and ticks > (rec.inserted_tick or 0) then
            reorder_after_trail(rec,"automatic")
        end
    elseif active_panel_key then
        local previous=installed[active_panel_key]
        if previous and valid(previous.menu)
            and safe(function() return previous.menu:IsInViewport() end)==false then
            if not pending_apply[previous.menu_key] then discard(previous.menu_key,"viewport_exit") end
            active_panel_key=nil
        end
    end
    if automatic and ticks%3==0 then add_rows("automatic") end
    if ticks%12==0 then
        for k,entry in pairs(installed) do
            if not valid(entry.panel) or not valid(entry.container) then
                installed[k]=nil
                if active_panel_key==k then active_panel_key=nil end
            end
        end
        for k,entry in pairs(sessions) do
            if not valid(entry.menu) then sessions[k]=nil;pending_apply[k]=nil end
        end
    end
end
local function start_monitor()
    if type(LoopInGameThreadWithDelay)=="function" then
        if pcall(function()
            LoopInGameThreadWithDelay(monitor_interval_ms,function()
                local ok,err=pcall(monitor_tick)
                if not ok then report("MONITOR_ERROR: "..tostring(err)) end
            end)
        end) then backend="game_thread_timer" end
    end
    if backend=="none" and type(LoopAsync)=="function" then
        if pcall(function()
            LoopAsync(monitor_interval_ms,function()
                ExecuteInGameThread(function()
                    local ok,err=pcall(monitor_tick)
                    if not ok then report("MONITOR_ERROR: "..tostring(err)) end
                end)
                return false
            end)
        end) then backend="async_dispatch" end
    end
    report("MONITOR: backend="..backend.." interval_ms="..monitor_interval_ms)
end
-- UI is installed automatically. No development hotkeys are registered.
print(PREFIX .. "v1.1.0 Settings UI loading\n")
report("v1.1.0 loaded: Interface settings and Apply-only INI persistence")
install_hooks("startup")
start_monitor()
