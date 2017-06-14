#include "engine/effects/engineeffect.h"

#include "util/sample.h"

EngineEffect::EngineEffect(const EffectManifest& manifest,
                           const QSet<ChannelHandleAndGroup>& registeredInputChannels,
                           const QSet<ChannelHandleAndGroup>& registeredOutputChannels,
                           EffectInstantiatorPointer pInstantiator)
        : m_manifest(manifest),
          m_parameters(manifest.parameters().size()) {
    const QList<EffectManifestParameter>& parameters = m_manifest.parameters();
    for (int i = 0; i < parameters.size(); ++i) {
        const EffectManifestParameter& parameter = parameters.at(i);
        EngineEffectParameter* pParameter =
                new EngineEffectParameter(parameter);
        m_parameters[i] = pParameter;
        m_parametersById[parameter.id()] = pParameter;
    }

    for (const ChannelHandleAndGroup& inputChannel : registeredInputChannels) {
        ChannelHandleMap<EffectProcessor::EnableState> outputChannelMap;
        for (const ChannelHandleAndGroup& outputChannel : registeredOutputChannels) {
            outputChannelMap.insert(outputChannel.handle(), EffectProcessor::DISABLED);
        }
        m_enableStateMatrix.insert(inputChannel.handle(), outputChannelMap);
    }

    // Creating the processor must come last.
    m_pProcessor = pInstantiator->instantiate(this, manifest);
    m_pProcessor->initialize(registeredInputChannels, registeredOutputChannels);
    m_effectRampsFromDry = manifest.effectRampsFromDry();
}

EngineEffect::~EngineEffect() {
    if (kEffectDebugOutput) {
        qDebug() << debugString() << "destroyed";
    }
    delete m_pProcessor;
    m_parametersById.clear();
    for (int i = 0; i < m_parameters.size(); ++i) {
        EngineEffectParameter* pParameter = m_parameters.at(i);
        m_parameters[i] = NULL;
        delete pParameter;
    }
}

bool EngineEffect::processEffectsRequest(const EffectsRequest& message,
                                         EffectsResponsePipe* pResponsePipe) {
    EngineEffectParameter* pParameter = NULL;
    EffectsResponse response(message);

    switch (message.type) {
        case EffectsRequest::SET_EFFECT_PARAMETERS:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << "SET_EFFECT_PARAMETERS"
                         << "enabled" << message.SetEffectParameters.enabled;
            }

            for (auto& outputMap : m_enableStateMatrix) {
                for (auto& enableState : outputMap) {
                    if (enableState != EffectProcessor::DISABLED
                        && !message.SetEffectParameters.enabled) {
                        enableState = EffectProcessor::DISABLING;
                    // If an input is not routed to the chain, and the effect gets
                    // a message to disable, then the effect gets the message to enable,
                    // process() will not have executed, so the enableState will still be
                    // DISABLING instead of DISABLED.
                    } else if ((enableState == EffectProcessor::DISABLED ||
                               enableState == EffectProcessor::DISABLING)
                               && message.SetEffectParameters.enabled) {
                        enableState = EffectProcessor::ENABLING;
                    }
                }
            }

            response.success = true;
            pResponsePipe->writeMessages(&response, 1);
            return true;
            break;
        case EffectsRequest::SET_PARAMETER_PARAMETERS:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << "SET_PARAMETER_PARAMETERS"
                         << "parameter" << message.SetParameterParameters.iParameter
                         << "minimum" << message.minimum
                         << "maximum" << message.maximum
                         << "default_value" << message.default_value
                         << "value" << message.value;
            }
            pParameter = m_parameters.value(
                message.SetParameterParameters.iParameter, NULL);
            if (pParameter) {
                pParameter->setMinimum(message.minimum);
                pParameter->setMaximum(message.maximum);
                pParameter->setDefaultValue(message.default_value);
                pParameter->setValue(message.value);
                response.success = true;
            } else {
                response.success = false;
                response.status = EffectsResponse::NO_SUCH_PARAMETER;
            }
            pResponsePipe->writeMessages(&response, 1);
            return true;
        default:
            break;
    }
    return false;
}

void EngineEffect::process(const ChannelHandle& inputHandle,
                           const ChannelHandle& outputHandle,
                           const CSAMPLE* pInput, CSAMPLE* pOutput,
                           const unsigned int numSamples,
                           const unsigned int sampleRate,
                           const EffectProcessor::EnableState chainEnableState,
                           const GroupFeatureState& groupFeatures) {
    // Compute the effective enable state from the combination of the effect's state
    // and the state passed from the EngineEffectChain
    EffectProcessor::EnableState effectiveEnableState = m_enableStateMatrix[inputHandle][outputHandle];
    if (chainEnableState == EffectProcessor::DISABLING) {
        effectiveEnableState = EffectProcessor::DISABLING;
    } else if (chainEnableState == EffectProcessor::ENABLING) {
        effectiveEnableState = EffectProcessor::ENABLING;
    }

    m_pProcessor->process(inputHandle, outputHandle, pInput, pOutput,
                          numSamples, sampleRate,
                          effectiveEnableState, groupFeatures);
    if (!m_effectRampsFromDry) {
        // the effect does not fade, so we care for it
        if (effectiveEnableState == EffectProcessor::DISABLING) {
            DEBUG_ASSERT(pInput != pOutput); // Fade to dry only works if pInput is not touched by pOutput
            // Fade out (fade to dry signal)
            SampleUtil::copy2WithRampingGain(pOutput,
                    pInput, 0.0, 1.0,
                    pOutput, 1.0, 0.0,
                    numSamples);
        } else if (effectiveEnableState == EffectProcessor::ENABLING) {
            DEBUG_ASSERT(pInput != pOutput); // Fade to dry only works if pInput is not touched by pOutput
            // Fade in (fade to wet signal)
            SampleUtil::copy2WithRampingGain(pOutput,
                    pInput, 1.0, 0.0,
                    pOutput, 0.0, 1.0,
                    numSamples);
        }
    }

    EffectProcessor::EnableState& effectOnChannelState = m_enableStateMatrix[inputHandle][outputHandle];
    if (effectOnChannelState == EffectProcessor::DISABLING) {
        effectOnChannelState = EffectProcessor::DISABLED;
    } else if (effectOnChannelState == EffectProcessor::ENABLING) {
        effectOnChannelState = EffectProcessor::ENABLED;
    }
}
