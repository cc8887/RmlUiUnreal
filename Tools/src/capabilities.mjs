import catalog from './capabilities.json' with { type: 'json' };

export const capabilityCatalog = catalog;
export function resolveProfile(name = 'legacy') {
  const profile = catalog.profiles[name];
  if (!profile) throw new Error(`Unknown RmlUi capability profile: ${name}`);
  return profile;
}

export function createCapabilities(profile, requiredFeatures = [], degradedFeatures = []) {
  const definition = resolveProfile(profile);
  const required = [...new Set(requiredFeatures)].sort();
  const degraded = [...new Set(degradedFeatures)].sort();
  for (const feature of [...required, ...degraded]) {
    if (!Object.hasOwn(catalog.features, feature)) throw new Error(`Unknown RmlUi capability: ${feature}`);
  }
  if (profile !== 'legacy') {
    for (const feature of required) {
      if (!definition.features.includes(feature)) throw new Error(`${profile} does not provide required capability ${feature}`);
    }
  }
  return {
    schemaVersion: catalog.schemaVersion, compiler: catalog.compiler, profile,
    minimumHostAbi: catalog.minimumHostAbi,
    ...(definition.minimumSlateAbi ? { minimumSlateAbi: definition.minimumSlateAbi } : {}),
    requiredFeatures: required, degradedFeatures: degraded,
  };
}

export function mergeCapabilities(profile, records, requiredFeatures = []) {
  return createCapabilities(profile,
    [...requiredFeatures, ...records.flatMap(item => item.requiredFeatures)],
    records.flatMap(item => item.degradedFeatures));
}
