local function passenger_sim()
    while(true) do
        asleep(8000)
        LOG(INFO, "approaching vehicle...")
        signal.set("passenger.approaching", 1)
        asleep(30000)
        LOG(INFO, "passenger absent...")
        signal.set("passenger.approaching", 0)
    end
end

local function env_sim()
    while(true) do
        asleep(10000)
        LOG(INFO, "safe to open door...")
        signal.set("env.safe.to.open", 1)
        asleep(30000)
        LOG(INFO, "passenger absent...")
        signal.set("env.safe.to.open", 0)
    end
end



function init()
    co1 = coroutine.create(passenger_sim)
    co2 = coroutine.create(env_sim)
end

function loop()
    if coroutine.status(co1) ~= 'dead' then
        local success, errorMsg = coroutine.resume(co1)
        if not success then
            error(errorMsg) -- Propagate the error up
        end
    end

    if coroutine.status(co2) ~= 'dead' then
        local success, errorMsg = coroutine.resume(co2)
        if not success then
            error(errorMsg) -- Propagate the error up
        end
    end
end
