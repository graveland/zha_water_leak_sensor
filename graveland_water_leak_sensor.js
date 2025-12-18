const {iasZoneAlarm, identify, numeric, onOff} = require('zigbee-herdsman-converters/lib/modernExtend');
const ota = require('zigbee-herdsman-converters/lib/ota');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const e = exposes.presets;
const ea = exposes.access;

const fzLocal = {
    reset_count: {
        cluster: 'haDiagnostic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.numberOfResets !== undefined) {
                return {reset_count: msg.data.numberOfResets};
            }
        },
    },
};

const definition = {
    zigbeeModel: ['Water Leak Sensor'],
    model: 'Water Leak Sensor',
    vendor: 'graveland',
    description: 'Water leak sensor (mains powered router)',
    extend: [
        iasZoneAlarm({
            zoneType: 'water_leak',
            zoneAttributes: ['alarm_1'],
        }),
        identify(),
        numeric({
            name: 'suppressed_changes',
            cluster: 'ssIasZone',
            attribute: {ID: 0xC000, type: 0x23},
            description: 'Cumulative count of suppressed state changes',
            reporting: {min: 60, max: 3600, change: 1},
            access: 'STATE_GET',
        }),
    ],
    exposes: [
        e.switch_().withEndpoint('reboot'),
        e.numeric('reset_count', ea.STATE).withDescription('Number of device resets'),
    ],
    fromZigbee: [
        fz.on_off,
        fzLocal.reset_count,
    ],
    toZigbee: [
        tz.on_off,
    ],
    endpoint: (device) => {
        return {reboot: 10};
    },
    ota: {
        isUpdateAvailable: async (device, logger, data = null) => {
            return ota.isUpdateAvailable(device, logger, data, {
                imageBlockResponseDelay: 250,
            });
        },
        updateToLatest: async (device, logger, onProgress) => {
            return ota.updateToLatest(device, logger, onProgress, {
                imageBlockResponseDelay: 250,
            });
        },
    },
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(1);

        // Configure reboot switch endpoint
        const rebootEndpoint = device.getEndpoint(10);
        if (rebootEndpoint) {
            await rebootEndpoint.bind('genOnOff', coordinatorEndpoint);
            await rebootEndpoint.read('genOnOff', ['onOff']);
        }

        // Read reset count from diagnostics cluster
        await endpoint.read('haDiagnostic', ['numberOfResets']);
    },
};

module.exports = definition;
