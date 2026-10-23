const {iasZoneAlarm, identify, numeric, onOff} = require('zigbee-herdsman-converters/lib/modernExtend');
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
    rejoin_count: {
        cluster: 'haDiagnostic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            // Manufacturer-specific 0xC001; zigbee-herdsman may key unknown
            // attributes by hex string or by decimal value.
            for (const key of ['0xc001', '0xC001', '49153']) {
                if (msg.data[key] !== undefined) {
                    return {rejoin_count: msg.data[key]};
                }
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
        e.switch_().withState('state', true).withEndpoint('reboot'),
        e.numeric('reset_count', ea.STATE).withDescription('Number of device resets'),
        e
            .switch_()
            .withState('state', true)
            .withEndpoint('rejoin')
            .withDescription('Leave the network and re-steer (send TOGGLE)'),
        e.numeric('rejoin_count', ea.STATE).withDescription('Number of forced re-joins'),
    ],
    fromZigbee: [
        fz.on_off,
        fzLocal.reset_count,
        fzLocal.rejoin_count,
    ],
    toZigbee: [
        tz.on_off,
    ],
    endpoint: (device) => {
        return {reboot: 10, rejoin: 11};
    },
    ota: true,
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
