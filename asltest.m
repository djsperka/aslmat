function asltest(addr, n)
%ASLTEST Connect to the ASL eye tracker and print eye data samples.
%   ASLTEST(ADDR, N) connects to the ASL eye tracker at ADDR, a char array
%   of the form 'addr:port', then calls ASL('eyepos') N times and prints
%   each sample on a single line. It waits 1/120 s between calls, which
%   matches the rate at which the asl background thread reads samples.
%   The connection is closed when ASLTEST finishes, errors, or is
%   interrupted with Ctrl+C.
%
%   Example:
%       asltest('128.120.140.228:51000', 100);
%
%   See also ASL.

    arguments
        addr (1,:) char
        n (1,1) double {mustBeInteger, mustBeNonnegative}
    end

    asl('connect', addr);
    cleanup = onCleanup(@() disconnectQuietly());
    waitForFirstSample(2);

    for i = 1:n
        s = asl('eyepos');
        fprintf(['%d: status=%d calibration_point_number=%d frame_number=%d ' ...
            'pupil_diameter=%d gaze_x=%g gaze_y=%g\n'], i, s.status, ...
            s.calibration_point_number, s.frame_number, s.pupil_diameter, ...
            s.gaze_x, s.gaze_y);
        pause(1/120);
    end
end

function waitForFirstSample(timeout)
    % asl('eyepos') errors until the background thread has read a sample.
    t = tic;
    while true
        try
            asl('eyepos');
            return;
        catch e
            if ~contains(e.message, 'No eye data received yet') || toc(t) > timeout
                rethrow(e);
            end
        end
        pause(0.01);
    end
end

function disconnectQuietly()
    % The connection may already be closed, e.g. if the tracker dropped it.
    try
        asl('disconnect');
    catch
    end
end
