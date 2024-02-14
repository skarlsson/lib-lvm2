--https://www.digital.auto/use-cases
--https://digitalauto.netlify.app/model/STLWzk1WyqVVLbfymb4f/library/prototype/sYi6V79NGZoxJW6AOKlG/view/code

local function passenger_welcome()
    asleep(3000)
    LOG(INFO, "Opening car door...")
    repeat
        actuator.set("vehicle.Cabin.Door.Row1.Left.IsOpen", 1)
    until(await(actuator.get("vehicle.Cabin.Door.Row1.Left.IsOpen") == 1))

    asleep(3000)
    LOG(INFO, "Turn Dome Light On")
    repeat
        actuator.set("vehicle.Cabin.Lights.IsDomeOn", 1)
    until(await(actuator.get("vehicle.Cabin.Lights.IsDomeOn") == 1))

    asleep(3000)
    LOG(INFO, "Raising Seat")
    repeat
        actuator.set("vehicle.Cabin.Seat.Row1.Pos1.Height", 100)
    until(await(actuator.get("vehicle.Cabin.Seat.Row1.Pos1.Height") > 80))
    asleep(3000)
    LOG(INFO, "Greeting done...")
    return true
end

--co = nil

function init()
    co = coroutine.create(passenger_welcome)
end

function loop()
    if coroutine.status(co) ~= 'dead' then
        local success, errorMsg = coroutine.resume(co)
        if not success then
            error(errorMsg) -- Propagate the error up
        end
    end
end
