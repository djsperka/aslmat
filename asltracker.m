classdef asltracker < handle
%ASLTRACKER Connection to an ASL eye tracker.
%   T = ASLTRACKER('address', ADDR, 'screen_dimensions', DIMS,
%       'screen_pixels', PIX, 'screen_distance', DIST) creates a tracker
%   object and connects to the ASL eye tracker. Once connected, the asl
%   MEX file reads eye data in the background at 120Hz. All arguments are
%   required:
%
%       'address'            Tracker address, a char array of the form
%                            'addr:port', where addr is an IP address or
%                            hostname.
%       'screen_dimensions'  [width height] of the screen's active area.
%                            Any length unit, e.g. cm.
%       'screen_pixels'      [width height] of the screen in pixels.
%       'screen_distance'    Distance from the eye to the screen, in the
%                            same units as 'screen_dimensions'.
%
%   Raises an error if an argument is missing or invalid, if a connection
%   is already open, or if the connection cannot be made.
%
%   ASLTRACKER methods:
%       eyepos     - Return the most recent eye data sample as a struct
%       calibrate  - Follow the tracker through a calibration
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
%           'screen_dimensions', [52 29.3], 'screen_pixels', [1920 1080], ...
%           'screen_distance', 57);
%       s = t.eyepos(2);    % wait up to 2 s for the first sample
%       fprintf('gaze = (%g, %g)\n', s.gaze_x, s.gaze_y);
%       t.disconnect();
%
%   See also ASL.

    properties (SetAccess = private)
        Address (1,:) char      % Tracker address, 'addr:port'
        ScreenDimensions        % [width height] of the screen's active area, any length unit
        ScreenPixels            % [width height] of the screen in pixels
        ScreenDistance          % Eye-to-screen distance, same units as ScreenDimensions
    end

    properties (Access = private)
        OwnsConnection (1,1) logical = false    % true if this object opened the current connection
    end

    methods
        function obj = asltracker(options)
            arguments
                options.address (1,:) char {mustBeNonempty}
                options.screen_dimensions (1,2) double {mustBePositive, mustBeFinite}
                options.screen_pixels (1,2) double {mustBePositive, mustBeInteger}
                options.screen_distance (1,1) double {mustBePositive, mustBeFinite}
            end
            required = {'address', 'screen_dimensions', 'screen_pixels', 'screen_distance'};
            missing = required(~isfield(options, required));
            if ~isempty(missing)
                error('asltracker:missingArgument', ...
                    'asltracker requires these arguments: %s. Missing: %s.', ...
                    strjoin(required, ', '), strjoin(missing, ', '));
            end
            obj.Address = options.address;
            obj.ScreenDimensions = options.screen_dimensions;
            obj.ScreenPixels = options.screen_pixels;
            obj.ScreenDistance = options.screen_distance;

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

        function calibrate(obj, window, timeout)
            %CALIBRATE Follow the tracker through a calibration.
            %   T.CALIBRATE(WINDOW) waits until the tracker reports a
            %   non-zero calibration point number, then follows the
            %   calibration as it moves from point to point. WINDOW is a
            %   Psychtoolbox window pointer. Returns when the calibration
            %   point number goes back to 0.
            %
            %   T.CALIBRATE(WINDOW, TIMEOUT) waits up to TIMEOUT seconds
            %   for the calibration point number to become non-zero, and
            %   raises an error if it doesn't. The default is 10.
            arguments
                obj
                window (1,1) double
                timeout (1,1) double {mustBeNonnegative} = 10
            end

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

            % Follow the calibration. The first point counts as a change.
            currentPoint = 0;
            while s.calibration_point_number ~= 0
                if s.calibration_point_number ~= currentPoint
                    currentPoint = s.calibration_point_number;
                    fprintf('cal pt %d\n', currentPoint);
                    % TODO: The calibration point changed to currentPoint.
                    % Add code here to respond, e.g. draw the target for
                    % currentPoint in window.
                end
                pause(period);
                s = obj.eyepos();
            end
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
