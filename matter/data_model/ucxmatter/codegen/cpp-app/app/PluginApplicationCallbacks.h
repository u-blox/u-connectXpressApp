#pragma once
void MatterAccessControlPluginServerInitCallback();
void MatterAdministratorCommissioningPluginServerInitCallback();
void MatterBasicInformationPluginServerInitCallback();
void MatterBooleanStatePluginServerInitCallback();
void MatterDescriptorPluginServerInitCallback();
void MatterGeneralCommissioningPluginServerInitCallback();
void MatterGeneralDiagnosticsPluginServerInitCallback();
void MatterGroupKeyManagementPluginServerInitCallback();
void MatterGroupsPluginServerInitCallback();
void MatterIdentifyPluginServerInitCallback();
void MatterLevelControlPluginServerInitCallback();
void MatterNetworkCommissioningPluginServerInitCallback();
void MatterOnOffPluginServerInitCallback();
void MatterOperationalCredentialsPluginServerInitCallback();
void MatterTemperatureMeasurementPluginServerInitCallback();
void MatterAccessControlPluginServerShutdownCallback();
void MatterAdministratorCommissioningPluginServerShutdownCallback();
void MatterBasicInformationPluginServerShutdownCallback();
void MatterBooleanStatePluginServerShutdownCallback();
void MatterDescriptorPluginServerShutdownCallback();
void MatterGeneralCommissioningPluginServerShutdownCallback();
void MatterGeneralDiagnosticsPluginServerShutdownCallback();
void MatterGroupKeyManagementPluginServerShutdownCallback();
void MatterGroupsPluginServerShutdownCallback();
void MatterIdentifyPluginServerShutdownCallback();
void MatterLevelControlPluginServerShutdownCallback();
void MatterNetworkCommissioningPluginServerShutdownCallback();
void MatterOnOffPluginServerShutdownCallback();
void MatterOperationalCredentialsPluginServerShutdownCallback();
void MatterTemperatureMeasurementPluginServerShutdownCallback();

#define MATTER_PLUGINS_INIT                                                    \
  MatterAccessControlPluginServerInitCallback();                               \
  MatterAdministratorCommissioningPluginServerInitCallback();                  \
  MatterBasicInformationPluginServerInitCallback();                            \
  MatterBooleanStatePluginServerInitCallback();                                \
  MatterDescriptorPluginServerInitCallback();                                  \
  MatterGeneralCommissioningPluginServerInitCallback();                        \
  MatterGeneralDiagnosticsPluginServerInitCallback();                          \
  MatterGroupKeyManagementPluginServerInitCallback();                          \
  MatterGroupsPluginServerInitCallback();                                      \
  MatterIdentifyPluginServerInitCallback();                                    \
  MatterLevelControlPluginServerInitCallback();                                \
  MatterNetworkCommissioningPluginServerInitCallback();                        \
  MatterOnOffPluginServerInitCallback();                                       \
  MatterOperationalCredentialsPluginServerInitCallback();                      \
  MatterTemperatureMeasurementPluginServerInitCallback();

#define MATTER_PLUGINS_SHUTDOWN                                                \
  MatterAccessControlPluginServerShutdownCallback();                           \
  MatterAdministratorCommissioningPluginServerShutdownCallback();              \
  MatterBasicInformationPluginServerShutdownCallback();                        \
  MatterBooleanStatePluginServerShutdownCallback();                            \
  MatterDescriptorPluginServerShutdownCallback();                              \
  MatterGeneralCommissioningPluginServerShutdownCallback();                    \
  MatterGeneralDiagnosticsPluginServerShutdownCallback();                      \
  MatterGroupKeyManagementPluginServerShutdownCallback();                      \
  MatterGroupsPluginServerShutdownCallback();                                  \
  MatterIdentifyPluginServerShutdownCallback();                                \
  MatterLevelControlPluginServerShutdownCallback();                            \
  MatterNetworkCommissioningPluginServerShutdownCallback();                    \
  MatterOnOffPluginServerShutdownCallback();                                   \
  MatterOperationalCredentialsPluginServerShutdownCallback();                  \
  MatterTemperatureMeasurementPluginServerShutdownCallback();
