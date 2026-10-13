classdef asltracker < handle
%ASLTRACKER Connection to an ASL eye tracker.
%   T = ASLTRACKER('address', ADDR, 'screen_dimensions', DIMS,
%       'screen_rect', RECT, 'screen_distance', DIST) creates a tracker
%   object and connects to the ASL eye tracker. Once connected, the asl
%   MEX file reads eye data in the background at 120Hz. All arguments are
%   required:
%
%       'address'            Tracker address, a char array of the form
%                            'addr:port', where addr is an IP address or
%                            hostname.
%       'screen_dimensions'  [width height] of the screen's active area.
%                            Any length unit, e.g. cm.
%       'screen_rect'        Screen rect in pixels, [left top right bottom],
%                            as returned by Screen('Rect', window).
%       'screen_distance'    Distance from the eye to the screen, in the
%                            same units as 'screen_dimensions'.
%
%   Raises an error if an argument is missing or invalid, if a connection
%   is already open, or if the connection cannot be made.
%
%   ASLTRACKER methods:
%       eyepos     - Return the most recent eye data sample as a struct
%       calibrate  - Follow the tracker through a calibration
%       message    - Send a message value (bits 0-13 of XDAT)
%       start_recording - Set the recording bits (14-15 of XDAT) and send
%       stop_recording  - Clear the recording bits (14-15 of XDAT) and send
%       disconnect - Close the connection
%       connect    - Reconnect after DISCONNECT, or after the tracker
%                    dropped the connection
%
%   ASLTRACKER uses the asl MEX file, which supports one connection at a
%   time, so only one ASLTRACKER object can be connected at once.
%
%   Deleting the object, or clearing the last variable that refers to it,
%   closes its connection.
%
%   Example:
%       t = asltracker('address', '128.120.140.228:51000', ...
%           'screen_dimensions', [52 29.3], 'screen_rect', [0 0 1920 1080], ...
%           'screen_distance', 57);
%       s = t.eyepos(2);    % wait up to 2 s for the first sample
%       fprintf('gaze = (%g, %g)\n', s.gaze_x, s.gaze_y);
%       t.disconnect();
%
%   See also ASL.

    properties (SetAccess = private)
        Address (1,:) char      % Tracker address, 'addr:port'
        ScreenDimensions        % [width height] of the screen's active area, any length unit
        ScreenRect              % Screen rect in pixels, [left top right bottom]
        ScreenDistance          % Eye-to-screen distance, same units as ScreenDimensions
    end

    properties (Access = private)
        OwnsConnection (1,1) logical = false    % true if this object opened the current connection
        Xdat (1,1) uint16                       % Value last sent with asl('xdat'). Bits 0-13: message, bits 14-15: recording
    end

    properties (Constant, Access = private)
        MessageMask = uint16(0x3FFF)            % Xdat bits 0-13
        RecordingMask = uint16(0xC000)          % Xdat bits 14-15
    end

    methods
        function obj = asltracker(options)
            arguments
                options.address (1,:) char {mustBeNonempty}
                options.screen_dimensions (1,2) double {mustBePositive, mustBeFinite}
                options.screen_rect (1,4) {mustBeNumeric, mustBeReal, mustBeFinite, mustBeValidRect}
                options.screen_distance (1,1) double {mustBePositive, mustBeFinite}
            end
            required = {'address', 'screen_dimensions', 'screen_rect', 'screen_distance'};
            missing = required(~isfield(options, required));
            if ~isempty(missing)
                error('asltracker:missingArgument', ...
                    'asltracker requires these arguments: %s. Missing: %s.', ...
                    strjoin(required, ', '), strjoin(missing, ', '));
            end
            obj.Address = options.address;
            obj.ScreenDimensions = options.screen_dimensions;
            obj.ScreenRect = double(options.screen_rect);
            obj.ScreenDistance = options.screen_distance;
            obj.Xdat = uint16(0);

            obj.connect();
        end

        function connect(obj)
            %CONNECT Reconnect to the tracker.
            %   T.CONNECT() connects to the tracker at T.Address. The
            %   constructor connects, so you only need this after
            %   DISCONNECT, or after the tracker dropped the connection.
            %   Raises an error if a connection is already open or the
            %   connection cannot be made.
            asl('connect', obj.Address);
            obj.OwnsConnection = true;
        end

        function s = eyepos(obj, timeout)
            %EYEPOS Return the most recent eye data sample.
            %   S = T.EYEPOS() returns the most recent sample as a struct
            %   with fields status, calibration_point_number, frame_number,
            %   pupil_diameter, gaze_x and gaze_y. All values are doubles.
            %
            %   S = T.EYEPOS(TIMEOUT) waits up to TIMEOUT seconds for the
            %   first sample to arrive after connecting. The default is 0,
            %   which raises an error at once if no sample has arrived yet.
            arguments
                obj
                timeout (1,1) double {mustBeNonnegative} = 0
            end
            t = tic;
            while true
                try
                    s = asl('eyepos');
                    return;
                catch e
                    if ~contains(e.message, 'No eye data received yet') || toc(t) >= timeout
                        rethrow(e);
                    end
                end
                pause(0.01);
            end
        end

        function calibrate(obj, window, fixpt, timeout)
            %CALIBRATE Follow the tracker through a calibration.
            %   T.CALIBRATE(WINDOW, FIXPT) waits until the tracker reports
            %   a non-zero calibration point number, then follows the
            %   calibration as it moves from point to point. Each time the
            %   point changes, FIXPT is moved to the new point, drawn in
            %   WINDOW and shown. WINDOW is a Psychtoolbox window pointer
            %   and FIXPT is a FixationPoint. Returns when the calibration
            %   point number goes back to 0.
            %
            %   T.CALIBRATE(WINDOW, FIXPT, TIMEOUT) waits up to TIMEOUT
            %   seconds for the calibration point number to become
            %   non-zero, and raises an error if it doesn't. The default
            %   is 10.
            arguments
                obj
                window (1,1) double
                fixpt (1,1) FixationPoint
                timeout (1,1) double {mustBeNonnegative} = 10
            end

            % clear screen
            Screen('Flip', window);

            % Poll at the rate the asl background thread reads samples.
            period = 1/120;

            % Wait for the tracker to start calibrating. The timeout also
            % covers waiting for the first sample after connecting.
            t = tic;
            s = obj.eyepos(timeout);
            while s.calibration_point_number == 0
                if toc(t) >= timeout
                    error('asltracker:calibrationTimeout', ...
                        'Calibration did not start within %g seconds.', timeout);
                end
                pause(period);
                s = obj.eyepos();
            end

            % matrix to scale gaze coordinates to pixel coordinates
            screenWidthPixels = RectWidth(obj.ScreenRect);
            screenHeightPixels = RectHeight(obj.ScreenRect);
            % Convert normalized gaze coordinates to screen pixels.
            gazeToPixels = [screenWidthPixels/260, 0; 0, screenHeightPixels/255];
            gazeOffset = obj.ScreenRect(1:2);

            % Follow the calibration. The first point counts as a change.
            currentPoint = 0;
            while s.calibration_point_number ~= 0
                if s.calibration_point_number ~= currentPoint
                    currentPoint = s.calibration_point_number;
                    targetPosition = [s.gaze_x, s.gaze_y];
                    targetPixels = gazeToPixels * targetPosition(:) + gazeOffset(:);
                    fprintf('cal pt %d: %f, %f --> %f, %f\n', currentPoint, s.gaze_x, s.gaze_y, targetPixels(1), targetPixels(2));

                    % Move the fixation point to the target and show it.
                    % FixationPoint is a value class, so assign the result back.
                    fixpt.XY = targetPixels(:)';
                    fixpt.draw(window);
                    Screen('Flip', window);
                end
                pause(period);
                s = obj.eyepos();
            end
            % leave a blank screen when done
            Screen('Flip', window);
        end

        function message(obj, value)
            %MESSAGE Send a message value to the tracker.
            %   T.MESSAGE(VALUE) rounds VALUE to an integer and puts its
            %   bits 0-13 in bits 0-13 of the XDAT value, then sends XDAT
            %   to the tracker. Higher bits of VALUE are ignored, and bits
            %   14-15 of XDAT (the recording bits) are unchanged. Negative
            %   values are treated as two's complement.
            arguments
                obj
                value (1,1) {mustBeNumeric, mustBeReal, mustBeFinite}
            end
            % int64 rounds; typecast keeps the two's complement bits of negative values.
            bits = typecast(int64(value), 'uint64');
            low14 = uint16(bitand(bits, uint64(obj.MessageMask)));
            obj.Xdat = bitor(bitand(obj.Xdat, obj.RecordingMask), low14);
            asl('xdat', obj.Xdat);
        end

        function start_recording(obj)
            %START_RECORDING Tell the tracker to start recording.
            %   T.START_RECORDING() sets bits 14 and 15 of the XDAT value,
            %   leaving the message bits unchanged, and sends XDAT to the
            %   tracker.
            obj.Xdat = bitor(obj.Xdat, obj.RecordingMask);
            asl('xdat', obj.Xdat);
        end

        function stop_recording(obj)
            %STOP_RECORDING Tell the tracker to stop recording.
            %   T.STOP_RECORDING() clears bits 14 and 15 of the XDAT value,
            %   leaving the message bits unchanged, and sends XDAT to the
            %   tracker.
            obj.Xdat = bitand(obj.Xdat, obj.MessageMask);
            asl('xdat', obj.Xdat);
        end

        function disconnect(obj)
            %DISCONNECT Close the connection.
            %   T.DISCONNECT() stops reading eye data and closes the
            %   connection. Raises an error if not connected.
            obj.OwnsConnection = false;
            asl('disconnect');
        end

        function delete(obj)
            % Close the connection if this object opened it. It may already
            % be closed, e.g. if the tracker dropped it.
            if obj.OwnsConnection
                try
                    asl('disconnect');
                catch
                end
            end
        end
    end
end

function mustBeValidRect(rect)
    % A rect is [left top right bottom], with right > left and bottom > top.
    if rect(3) <= rect(1) || rect(4) <= rect(2)
        error('asltracker:invalidRect', ...
            'screen_rect must be [left top right bottom], with right > left and bottom > top.');
    end
end
