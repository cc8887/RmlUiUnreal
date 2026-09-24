import * as echarts from 'echarts/core';
import { BarChart, LineChart } from 'echarts/charts';
import { GridComponent } from 'echarts/components';
import { SVGRenderer } from 'echarts/renderers';

echarts.use([BarChart, LineChart, GridComponent, SVGRenderer]);

export interface EditableChartDatum {
  label: string;
  value: number;
  target: number;
}

export function renderEditableChartSvg(data: readonly EditableChartDatum[], width = 720, height = 300): string {
  const chart = echarts.init(null, null, { renderer: 'svg', ssr: true, width, height });
  try {
    chart.setOption({
      animation: false,
      backgroundColor: '#ffffff',
      color: ['#168c78', '#d69a2f'],
      textStyle: { color: '#45545e', fontFamily: 'Noto Sans CJK SC' },
      grid: { left: 48, right: 22, top: 20, bottom: 46 },
      xAxis: {
        type: 'category',
        data: data.map(item => item.label),
        axisLine: { lineStyle: { color: '#aebbc3' } },
        axisTick: { show: false },
        axisLabel: { color: '#596974', fontSize: 11 },
      },
      yAxis: {
        type: 'value', min: 0, max: 100, interval: 25,
        axisLabel: { color: '#71808a', fontSize: 10 },
        splitLine: { lineStyle: { color: '#e1e7ea' } },
      },
      series: [
        {
          name: 'Current', type: 'bar', data: data.map(item => item.value), barMaxWidth: 42,
          itemStyle: { color: '#168c78', borderRadius: [3, 3, 0, 0] },
        },
        {
          name: 'Target', type: 'line', data: data.map(item => item.target), symbol: 'circle', symbolSize: 7,
          lineStyle: { color: '#d69a2f', width: 2 }, itemStyle: { color: '#d69a2f' },
        },
      ],
    });
    return chart.renderToSVGString();
  } finally {
    chart.dispose();
  }
}
